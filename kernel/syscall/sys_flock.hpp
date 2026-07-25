/**
 * @file kernel/syscall/sys_flock.hpp
 * @brief sys_flock handler declaration (F6-M1 B2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::fs {
struct Inode;
}
namespace cinux::proc {
struct Task;
}

namespace cinux::syscall {

/// Kernel-internal flock: apply @p operation (LOCK_SH/EX/UN [+NB]) to @p inode
/// on behalf of @p owner. Ring-0 tests drive this directly with stack Task/Inode.
/// Returns 0 or -errno (-EAGAIN on NB conflict).
int64_t do_flock_kernel(cinux::fs::Inode* inode, cinux::proc::Task* owner, uint32_t operation);

/// Linux flock(2) (syscall 73): @p fd -> File -> inode; owner = current task.
int64_t sys_flock(uint64_t fd, uint64_t operation, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
