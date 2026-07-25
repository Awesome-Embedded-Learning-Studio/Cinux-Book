/**
 * @file kernel/syscall/sys_flock.cpp
 * @brief sys_flock handler (F6-M1 B2)
 *
 * Resolves @p fd to its File/Inode and delegates to FileLockManager with the
 * current task as the lock owner. The ring-internal do_flock_kernel takes the
 * Inode + Task explicitly so mechanism tests can drive it without a real fd
 * table (stack Task + stack Inode, like test_creds' do_ pattern).
 */

#include "kernel/syscall/sys_flock.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"         // File (via current_fd_table().get)
#include "kernel/fs/file_lock.hpp"    // FileLockManager
#include "kernel/fs/vfs_mount.hpp"    // current_fd_table
#include "kernel/proc/scheduler.hpp"  // Scheduler::current

namespace cinux::syscall {

int64_t do_flock_kernel(cinux::fs::Inode* inode, cinux::proc::Task* owner, uint32_t operation) {
    return cinux::fs::FileLockManager::flock(inode, owner, operation);
}

int64_t sys_flock(uint64_t fd, uint64_t operation, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::File* file = cinux::fs::current_fd_table().get(static_cast<int>(fd));
    if (file == nullptr) {
        return -kEbadf;
    }
    cinux::proc::Task* owner = cinux::proc::Scheduler::current();
    if (owner == nullptr) {
        return -kEinval;  // no current task -- cannot own a lock
    }
    return do_flock_kernel(file->inode, owner, static_cast<uint32_t>(operation));
}

}  // namespace cinux::syscall
