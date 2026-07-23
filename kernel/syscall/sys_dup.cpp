/**
 * @file kernel/syscall/sys_dup.cpp
 * @brief sys_dup / sys_dup2 handlers (F-ECO batch 4)
 *
 * Thin shims over FDTable::dup / dup2 (see sys_dup.hpp).  dup() distinguishes
 * -EBADF (bad oldfd) from -EMFILE (table full) with a get() precheck; the
 * get+dup pair is a benign TOCTOU under the table lock at hobby scale (a race
 * needs another thread closing the fd between the two calls).
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_dup.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"  // current_fd_table()

namespace cinux::syscall {

int64_t sys_dup(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::FDTable& tbl = cinux::fs::current_fd_table();
    if (tbl.get(static_cast<int>(fd)) == nullptr) {
        return -cinux::kEbadf;
    }
    int nfd = tbl.dup(static_cast<int>(fd), 0);
    return nfd >= 0 ? static_cast<int64_t>(nfd) : -cinux::kEmfile;
}

int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t, uint64_t, uint64_t, uint64_t) {
    int nfd = cinux::fs::current_fd_table().dup2(static_cast<int>(oldfd), static_cast<int>(newfd));
    return nfd >= 0 ? static_cast<int64_t>(nfd) : -cinux::kEbadf;
}

}  // namespace cinux::syscall
