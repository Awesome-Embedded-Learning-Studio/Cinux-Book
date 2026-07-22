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

namespace cinux::syscall {

int64_t sys_getuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_geteuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getgid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getegid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setuid(uint64_t uid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setgid(uint64_t gid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
