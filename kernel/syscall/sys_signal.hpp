/**
 * @file kernel/syscall/sys_signal.hpp
 * @brief Signal syscall handlers (F3-M1)
 *
 * kill / rt_sigaction / rt_sigprocmask.  rt_sigreturn arrives in batch 3
 * (it needs the signal-frame layout).  Handlers read/write user memory
 * directly: the kernel (ring 0) maps the caller's address space, so a
 * user pointer is accessible without a copy primitive (a bad pointer
 * faults and is handled by the PF path).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// User-space sigaction layout (subset of the Linux struct sigaction).
/// sa_handler: 0 = SIG_DFL, 1 = SIG_IGN, otherwise a handler entry address.
struct UserSigAction {
    uint64_t sa_handler;
    uint64_t sa_mask;
    uint64_t sa_flags;
    uint64_t sa_restorer;
};

/// Send signal @p sig to process @p pid (Linux syscall 62).
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t);

/// Install or query the disposition for @p sig (Linux syscall 13).
int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t, uint64_t, uint64_t);

/// Change or query the signal mask (Linux syscall 14).  @p how is
/// SIG_BLOCK(0) / SIG_UNBLOCK(1) / SIG_SETMASK(2).
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
