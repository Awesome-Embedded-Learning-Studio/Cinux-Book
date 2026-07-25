/**
 * @file kernel/syscall/sys_close.cpp
 * @brief sys_close handler implementation
 *
 * Closes a file descriptor by releasing the File entry in the
 * global FDTable.
 */

#include "kernel/syscall/sys_close.hpp"

#include <stdint.h>

#include "kernel/fs/file.hpp"
#include "kernel/fs/file_lock.hpp"     // FileLockManager (close releases flock)
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/proc/scheduler.hpp"   // Scheduler::current

namespace cinux::syscall {

int64_t sys_close(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // F6-M1 B2: release the current task's flock locks on this inode before
    // close detaches the File. Hobby-OS simplification: a CLONE_FILES shared
    // table can race the File pointer (dup2/close on another CPU); the common
    // single-task sys_close path does not. Task-exit flock cleanup is a follow-up.
    cinux::fs::FDTable& tbl = cinux::fs::current_fd_table();
    if (cinux::fs::File* file = tbl.get(static_cast<int>(fd))) {
        cinux::fs::FileLockManager::release_task_inode(file->inode,
                                                        cinux::proc::Scheduler::current());
    }
    return static_cast<int64_t>(tbl.close(static_cast<int>(fd)));
}

}  // namespace cinux::syscall
