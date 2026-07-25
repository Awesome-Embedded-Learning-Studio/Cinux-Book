/**
 * @file kernel/fs/file_lock.hpp
 * @brief FileLockManager -- whole-file POSIX advisory locks (flock(2), F6-M1 B2)
 *
 * flock(2): LOCK_SH (shared readers) / LOCK_EX (exclusive writer) / LOCK_UN,
 * with LOCK_NB for non-blocking.  Conflict is per-inode; the owner is the Task.
 * Linux tracks per-open-file-description owners (dup'd fds share one lock);
 * this is a hobby-OS simplification where close(fd) drops every lock that task
 * holds on the inode (the dup-shared-description semantics is deferred).
 *
 * Namespace: cinux::fs
 */
#pragma once

#include <stdint.h>

namespace cinux::proc {
struct Task;
}

namespace cinux::fs {

struct Inode;

/// flock(2) operation bits (Linux UAPI values).
static constexpr uint32_t kLockSh = 1;  ///< LOCK_SH (shared)
static constexpr uint32_t kLockEx = 2;  ///< LOCK_EX (exclusive)
static constexpr uint32_t kLockNb = 4;  ///< LOCK_NB (non-blocking; OR'd with SH/EX)
static constexpr uint32_t kLockUn = 8;  ///< LOCK_UN (release)

class FileLockManager {
public:
    /// Apply a flock(2) operation. @p inode is the lock key (two fds opening the
    /// same inode conflict); @p owner is the lock holder. Returns 0, -EAGAIN
    /// (LOCK_NB on conflict), or -EINVAL.
    static int64_t flock(Inode* inode, cinux::proc::Task* owner, uint32_t operation);

    /// Release every lock @p owner holds on @p inode. Hooked from FDTable::close
    /// so closing an fd drops that task's locks on the inode.
    static void release_task_inode(Inode* inode, cinux::proc::Task* owner);
};

}  // namespace cinux::fs
