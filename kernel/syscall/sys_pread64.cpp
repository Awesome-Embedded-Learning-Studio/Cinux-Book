/**
 * @file kernel/syscall/sys_pread64.cpp
 * @brief sys_pread64 handler (B4-B2)
 *
 * Positioned read: reads @p count bytes at @p offset from the fd's inode WITHOUT
 * advancing file->offset (POSIX pread). glibc's ldso uses this to precision-read
 * ELF segments/notes. Structured like sys_read: a kernel staging buffer is
 * filled via do_pread64_kernel (the only place that may block), then copy_to_user
 * opens the stac window once the task is runnable again.
 */

#include "kernel/syscall/sys_pread64.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // SMAP: copy_to_user
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/slab.hpp"  // kmalloc staging buffer

namespace cinux::syscall {

int64_t do_pread64_kernel(int fd, void* kbuf, uint64_t count, uint64_t offset) {
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(fd);
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        // Disk-backed files go through the PageCache (shared with demand paging);
        // transient ops keep their direct read path. @p offset comes from the
        // caller -- file->offset is intentionally NOT advanced (pread semantics).
        auto read_result =
            file->inode->ops->is_page_cacheable()
                ? cinux::mm::g_page_cache.read_bytes(file->inode, offset, kbuf, count)
                : file->inode->ops->read(file->inode, offset, kbuf, count);
        if (!read_result.ok()) {
            return -to_errno(read_result.error());
        }
        return read_result.value();
    }
    // pread on a non-seekable fd (stdin/console/pipe) is ESPIPE on Linux; we do
    // not host those as pread targets, so collapse to EBADF (as never preads fd 0).
    return -kEbadf;
}

int64_t sys_pread64(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t offset, uint64_t,
                    uint64_t) {
    if (!cinux::user::access_ok(reinterpret_cast<void*>(buf_virt), count)) {
        return -kEfault;
    }
    if (count == 0) {
        return 0;
    }

    // Kernel staging buffer: small onstack, large onheap (see sys_read). The
    // block (if any) happens inside do_pread64_kernel with AC=0; only the
    // copy_to_user below opens the stac window.
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

    int64_t n = do_pread64_kernel(static_cast<int>(fd), kbuf, count, offset);
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
