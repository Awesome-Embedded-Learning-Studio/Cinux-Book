/**
 * @file kernel/syscall/sys_creds.hpp
 * @brief Process-credential syscall handlers (F9 batch 9 / M3)
 *
 * getuid/getgid/geteuid/getegid return the real/effective IDs of the current
 * task. setuid/setgid change the effective ID under a simplified rule:
 * root (euid==0 / egid==0) may set it to anything; a non-root task may only
 * drop it back to its real ID. The Linux saved-set (allowing setuid to swap
 * real/effective repeatedly) and setuid-binary support (execve honoring
 * S_ISUID) are deferred to F6 alongside file-permission enforcement.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::proc {
struct Task;  // forward -- see kernel/proc/process.hpp
}

namespace cinux::syscall {

int64_t sys_getuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_geteuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getgid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getegid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setuid(uint64_t uid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setgid(uint64_t gid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// @name F-ECO batch 8: supplementary groups (getgroups/setgroups -- `id`/`newgrp`).
/// getgroups(0, NULL) returns the supplementary-group count; with a buffer it
/// fills up to @p size gid_t entries (size < count -> -EINVAL).  setgroups is
/// root-only (euid==0, the CAP_SETGID stand-in); count > NGROUPS_MAX -> -EINVAL.
///@{
int64_t sys_getgroups(uint64_t size, uint64_t list_virt, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setgroups(uint64_t size, uint64_t list_virt, uint64_t, uint64_t, uint64_t, uint64_t);
/// Tests call these (kernel gid_t arrays + an explicit Task* -- the test kernel
/// runs on the boot thread where Scheduler::current() is null, so the do_
/// variants take the task directly; sys_getgroups/setgroups resolve current()).
int64_t do_getgroups_kernel(const cinux::proc::Task* task, uint32_t* out_groups, uint32_t cap);
int64_t do_setgroups_kernel(cinux::proc::Task* task, const uint32_t* in_groups, uint32_t count);
///@}

}  // namespace cinux::syscall
