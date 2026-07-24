/**
 * @file kernel/syscall/sys_getdents.cpp
 * @brief sys_getdents handler (P0e SMAP-layered)
 *
 * Layered: do_getdents_kernel reads one directory entry into a KERNEL buffer
 * (may block on disk I/O, AC=0 safe); sys_getdents is the boundary that
 * copy_to_user's it. The old readdir directly into the user buffer is gone.
 */

#include "kernel/syscall/sys_getdents.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0e (SMAP): copy_to_user
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::syscall {

int64_t do_getdents_kernel(int fd, char* kname, uint64_t count) {
    cinux::fs::File* file = cinux::fs::current_fd_table().get(fd);
    if (file == nullptr || file->inode == nullptr || file->inode->ops == nullptr) {
        return -cinux::kEbadf;
    }
    auto g = file->offset_lock_.guard();
    auto dir_result = file->inode->ops->readdir(file->inode, file->offset, kname, count);
    if (!dir_result.ok()) {
        return -cinux::to_errno(dir_result.error());
    }
    if (dir_result.value() == 1) {
        file->offset++;
        uint64_t len = 0;
        while (len < count && kname[len] != '\0') {
            ++len;
        }
        return static_cast<int64_t>(len);
    }
    return dir_result.value();
}

int64_t sys_getdents(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t) {
    if (count == 0) {
        return -cinux::kEfault;
    }
    if (!cinux::user::access_ok(reinterpret_cast<void*>(buf_virt), count)) {
        return -cinux::kEfault;
    }

    // Stage the entry name in a kernel buffer (readdir may block on disk).
    uint32_t stage = (count < 256) ? static_cast<uint32_t>(count) : 256;
    char     kname[256];
    int64_t  n = do_getdents_kernel(static_cast<int>(fd), kname, stage);
    if (n <= 0) {
        return n;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(buf_virt), kname,
                                   static_cast<uint64_t>(n))) {
        return -cinux::kEfault;
    }
    return n;
}

}  // namespace cinux::syscall
