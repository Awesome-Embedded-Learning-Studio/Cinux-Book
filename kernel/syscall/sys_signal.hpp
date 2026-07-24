/**
 * @file kernel/syscall/sys_signal.hpp
 * @brief Signal syscall handlers (F3-M1)
 *
 * kill / rt_sigaction / rt_sigprocmask.  User sigaction/mask payloads cross
 * through copy_to_user/copy_from_user/get_user/put_user; the signal-frame
 * path in proc/signal.cpp follows the same SMAP/extable boundary rule.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/proc/signal.hpp"  // P0d: do_*_kernel use SigAction/SigSet

namespace cinux::proc {
struct Task;
}

namespace cinux::syscall {

/// User-space sigaction layout, matching the Linux x86_64 `struct sigaction`
/// (uapi/asm-generic/signal.h): { sa_handler, sa_flags, sa_restorer, sa_mask }.
/// musl constructs exactly this layout (src/signal/sigaction.c, k_sigaction) and
/// passes sigsetsize = _NSIG/8 = 8 as rt_sigaction's 4th arg, so sa_mask is the
/// 8-byte kernel sigset (signals 1..64). sa_handler: 0 = SIG_DFL, 1 = SIG_IGN,
/// otherwise a handler entry address. Cinux ignores sa_restorer (it injects its
/// own sigreturn trampoline) but the field must sit at the Linux offset.
struct UserSigAction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

/// Send signal @p sig to process @p pid (Linux syscall 62).
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t);

/// Install or query the disposition for @p sig (Linux syscall 13).
int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t, uint64_t, uint64_t);

/// Change or query the signal mask (Linux syscall 14).  @p how is
/// SIG_BLOCK(0) / SIG_UNBLOCK(1) / SIG_SETMASK(2).
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t, uint64_t, uint64_t);

/// Wait for a signal in @p set (Linux syscall 128, B3b).  Blocks until one of
/// the signals in the user sigset is pending, then dequeues it and returns the
/// signo.  @p info (optional) is zeroed (siginfo_t detail deferred).  @p timeout
/// is accepted but ignored (block until a signal lands) -- busybox init's
/// respawn loop only needs to wake on SIGCHLD.
int64_t sys_rt_sigtimedwait(uint64_t set, uint64_t info, uint64_t timeout, uint64_t, uint64_t,
                            uint64_t);

/// P0d (SMAP): pure kernel-to-kernel signal logic (no user memory). Tests and
/// kernel-internal callers use these; sys_* are the user boundaries.
int64_t do_kill_kernel(cinux::proc::Task* cur, int32_t pid, int sig);
int64_t do_sigaction_kernel(cinux::proc::Task* task, int sig, const cinux::proc::SigAction* new_act,
                            cinux::proc::SigAction* old_act);
int64_t do_sigprocmask_kernel(cinux::proc::Task* task, int how, const cinux::proc::SigSet* set,
                              cinux::proc::SigSet* oset);

}  // namespace cinux::syscall
