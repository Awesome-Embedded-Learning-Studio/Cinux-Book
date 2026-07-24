/**
 * @file kernel/syscall/sys_write.cpp
 * @brief sys_write handler implementation (P0b SMAP-layered)
 *
 * Layered (Linux-aligned):
 *   - do_write_kernel: pure kernel-to-kernel (fd + KERNEL buffer -> VFS write
 *     or fd=1 kprintf). May block (e.g. pipe full); never opens a stac window
 *     and never touches user memory. Tests call this with a kernel buffer.
 *   - sys_write: the user boundary. It stages the user buffer into a kernel
 *     buffer via copy_from_user (the only stac window), then calls
 *     do_write_kernel. fd=1's kprintf reads the kernel buffer, not the user
 *     pointer -- the legacy `buf[i]` raw dereference is gone.
 *
 * The block-then-write rule (RFLAGS.AC is per-CPU, context_switch does not save
 * it) is honoured: copy_from_user completes before do_write_kernel, which is
 * the only place that may block.
 */

#include "kernel/syscall/sys_write.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0b (SMAP): copy_from_user
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/slab.hpp"  // P0b: kmalloc staging buffer
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

// ============================================================
// do_write_kernel: pure kernel-to-kernel write logic (no user memory)
// ============================================================

int64_t do_write_kernel(int fd, const void* kbuf, uint64_t count) {
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(fd);
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        // offset_lock_ guards file->offset.  Only disk-backed (page_cacheable)
        // files use offset; their write path does not block on schedule.
        // Pipes/pty are streams whose write() may block (pipe full ->
        // schedule_blocked, or EPIPE -> SIGPIPE), so holding offset_lock_ across
        // it would deadlock (LOCKDEP: schedule-while-held).
        if (file->inode->ops->is_page_cacheable()) {
            auto g            = file->offset_lock_.guard();
            auto write_result = file->inode->ops->write(file->inode, file->offset, kbuf, count);
            if (!write_result.ok()) {
                return -to_errno(write_result.error());
            }
            if (write_result.value() > 0) {
                file->offset += static_cast<uint64_t>(write_result.value());
            }
            return write_result.value();
        }
        // Non-page-cacheable (pipe/pty/ramdisk): direct write, may block --
        // no offset_lock_.  Update offset unlocked (see read side above).
        auto write_result = file->inode->ops->write(file->inode, file->offset, kbuf, count);
        if (!write_result.ok()) {
            int err = to_errno(write_result.error());
            // F3-M1: writing to a pipe/fifo with no readers raises SIGPIPE.
            if (err == kEpipe) {
                auto* self = cinux::proc::Scheduler::current();
                if (self != nullptr) {
                    cinux::proc::signal_send(self, cinux::proc::Signal::kSigpipe);
                }
            }
            return -err;
        }
        // EINTR sentinel from a signal-interrupted blocking write (pipe full):
        // InodeOps returns -1 as a success value (it cannot extend lib::Error
        // without touching the Cinux-Base submodule).  Map to -EINTR.
        if (write_result.value() == static_cast<int64_t>(-1)) {
            return -kEintr;
        }
        if (write_result.value() > 0) {
            file->offset += static_cast<uint64_t>(write_result.value());
        }
        return write_result.value();
    }

    // fd=1/2 (stdout/stderr): legacy console output when no VFS entry is present.
    // Reads the kernel staging buffer, not a user pointer.
    if (fd == 1 || fd == 2) {
        const auto* p = reinterpret_cast<const char*>(kbuf);
        for (uint64_t i = 0; i < count; i++) {
            kprintf("%c", p[i]);
        }
        return static_cast<int64_t>(count);
    }

    // No VFS entry and not a legacy fd -- fail
    return -kEbadf;
}

// ============================================================
// sys_write boundary: accessor stages user buf -> kernel buf -> do_write_kernel
// ============================================================

int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t) {
    if (!cinux::user::access_ok(reinterpret_cast<void*>(buf_virt), count)) {
        return -kEfault;
    }
    if (count == 0) {
        return 0;
    }

    // Stage the user buffer into a kernel buffer: small on the stack, large on
    // the heap. copy_from_user is the only stac window; do_write_kernel (which
    // may block on a full pipe) runs with AC=0.
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

    int64_t rc;
    if (!cinux::user::copy_from_user(kbuf, reinterpret_cast<void*>(buf_virt), count)) {
        rc = -kEfault;
    } else {
        rc = do_write_kernel(static_cast<int>(fd), kbuf, count);
    }

    if (heap) {
        cinux::mm::kfree(kbuf);
    }
    return rc;
}

}  // namespace cinux::syscall
