/**
 * @file kernel/syscall/sys_read.cpp
 * @brief sys_read handler implementation (P0b SMAP-layered)
 *
 * Layered (Linux-aligned):
 *   - do_read_kernel: pure kernel-to-kernel (fd -> KERNEL buffer). For fd=0 it
 *     calls console_tty_read(kbuf), which blocks on the kernel buffer (AC=0
 *     safe -- the block-then-write rule). For other fds it reads through the
 *     VFS / PageCache into the kernel buffer.
 *   - sys_read: the user boundary. It runs do_read_kernel into a kernel staging
 *     buffer (the only place that may block), then copy_to_user the bytes read
 *     once the task is runnable again. The stac window is only inside
 *     copy_to_user and never spans the block.
 *
 * console_tty_read's signature is unchanged: do_read_kernel simply hands it a
 * kernel buffer instead of the user pointer, so the blocking line discipline
 * never touches user memory while AC may be 0.
 */

#include "kernel/syscall/sys_read.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0b (SMAP): copy_to_user
#include "kernel/drivers/tty/console_tty.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/slab.hpp"  // P0b: kmalloc staging buffer

namespace cinux::syscall {

// ============================================================
// do_read_kernel: pure kernel-to-kernel read logic (no user memory)
// ============================================================

int64_t do_read_kernel(int fd, void* kbuf, uint64_t count) {
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(fd);
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        auto g = file->offset_lock_.guard();
        (void)g;
        // Disk-backed files (ext2) are served through the PageCache so that
        // read() and demand paging share one cached copy; pipes and other
        // transient ops keep their direct read() path.
        auto read_result =
            file->inode->ops->is_page_cacheable()
                ? cinux::mm::g_page_cache.read_bytes(file->inode, file->offset, kbuf, count)
                : file->inode->ops->read(file->inode, file->offset, kbuf, count);
        if (!read_result.ok()) {
            return -to_errno(read_result.error());
        }
        if (read_result.value() > 0) {
            file->offset += static_cast<uint64_t>(read_result.value());
        }
        return read_result.value();
    }

    // fd=0 (stdin): read a cooked line through the console TTY line discipline
    // (F10-M3). console_tty_read() blocks until a line is committed or EOF (^D
    // on empty). It writes the KERNEL buffer; the block happens with AC=0.
    if (fd == 0) {
        return static_cast<int64_t>(
            cinux::drivers::console_tty().read(reinterpret_cast<char*>(kbuf), count));
    }

    // No VFS entry and not a legacy fd -- fail
    return -kEbadf;
}

// ============================================================
// sys_read boundary: do_read_kernel into kernel buf (may block) -> copy_to_user
// ============================================================

int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t) {
    if (!cinux::user::access_ok(reinterpret_cast<void*>(buf_virt), count)) {
        return -kEfault;
    }
    if (count == 0) {
        return 0;
    }

    // Kernel staging buffer: small on stack, large on heap. do_read_kernel
    // (which may block on console_tty / pipe / disk) writes this buffer while
    // AC=0; only the copy_to_user below opens the stac window, after the task
    // is runnable again.
    constexpr uint64_t kStackStage = 256;
    uint8_t            stack_buf[kStackStage];
    void*              kbuf = stack_buf;
    bool               heap = count > kStackStage;
    if (heap) {
        kbuf = cinux::mm::kmalloc(count);
        if (kbuf == nullptr) {
            return -cinux::kEnomem;
        }
    }

    int64_t n = do_read_kernel(static_cast<int>(fd), kbuf, count);
    if (n > 0) {
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(buf_virt), kbuf,
                                       static_cast<uint64_t>(n))) {
            if (heap) {
                cinux::mm::kfree(kbuf);
            }
            return -kEfault;
        }
    }

    if (heap) {
        cinux::mm::kfree(kbuf);
    }
    return n;
}

}  // namespace cinux::syscall
