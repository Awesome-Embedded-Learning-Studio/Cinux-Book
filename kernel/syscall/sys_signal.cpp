/**
 * @file kernel/syscall/sys_signal.cpp
 * @brief Signal syscall handler implementations (F3-M1 batch 2)
 *
 * kill / rt_sigaction / rt_sigprocmask.  Translation boundary: user
 * UserSigAction <-> kernel SigAction, and int errno returns.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_signal.hpp"

#include <stdint.h>

#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"

namespace cinux::syscall {

using cinux::proc::HandlerType;
using cinux::proc::Signal;
using cinux::proc::SigAction;
using cinux::proc::SigSet;
using cinux::proc::Task;
using cinux::proc::TaskState;

// Linux sigaction flags (uapi/asm-generic/signal.h; musl arch/x86_64/bits/signal.h).
// SA_RESTORER (0x04000000) is intentionally not honoured: CinuxOS injects its own
// sigreturn trampoline rather than calling the user-provided restorer.
constexpr uint64_t kSaRestart   = 0x10000000;
constexpr uint64_t kSaResethand = 0x80000000;

int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!cinux::proc::signal_valid(static_cast<int>(sig))) {
        return -22;  // EINVAL
    }
    Signal  s   = static_cast<Signal>(sig);
    Task*   cur = cinux::proc::Scheduler::current();
    int32_t p   = static_cast<int32_t>(pid);

    Task* target = nullptr;
    if (p == 0 || (cur != nullptr && p == cur->pid)) {
        target = cur;  // signal self
    } else if (p > 0) {
        target = cinux::proc::signal_find_task_by_pid(p);
    } else {
        // POSIX kill(-pgid, sig): signal the whole process group (F3-M3).
        int sent = cinux::proc::killpg(-p, s);
        if (sent == 0) {
            return -3;  // ESRCH: no such process group
        }
        return 0;
    }
    if (target == nullptr) {
        return -3;  // ESRCH
    }
    return cinux::proc::signal_send(target, s);
}

int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t, uint64_t, uint64_t) {
    if (!cinux::proc::signal_valid(static_cast<int>(sig))) {
        return -22;  // EINVAL
    }
    Signal s = static_cast<Signal>(sig);
    // SIGKILL/SIGSTOP cannot be caught or ignored.
    if (cinux::proc::signal_is_uncatchable(s)) {
        return -22;
    }
    Task* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return -22;
    }
    int n = static_cast<int>(sig);

    if (oact != 0) {
        auto*            uo  = reinterpret_cast<UserSigAction*>(oact);
        const SigAction& cur = task->sig_actions->actions[n];
        uo->sa_handler       = (cur.type == HandlerType::kIgnore)   ? 1
                               : (cur.type == HandlerType::kCustom) ? cur.handler_addr
                                                                    : 0;
        uo->sa_flags    = (cur.sa_restart ? kSaRestart : 0) | (cur.sa_resethand ? kSaResethand : 0);
        uo->sa_restorer = 0;  // CinuxOS injects its own sigreturn trampoline
        uo->sa_mask     = cur.sa_mask;
    }
    if (act != 0) {
        const auto* ua  = reinterpret_cast<const UserSigAction*>(act);
        SigAction&  dst = task->sig_actions->actions[n];
        if (ua->sa_handler == 0) {
            dst.type = HandlerType::kDefault;
        } else if (ua->sa_handler == 1) {
            dst.type = HandlerType::kIgnore;
        } else {
            dst.type         = HandlerType::kCustom;
            dst.handler_addr = ua->sa_handler;
        }
        dst.sa_restart   = (ua->sa_flags & kSaRestart) != 0;
        dst.sa_resethand = (ua->sa_flags & kSaResethand) != 0;
        dst.sa_mask      = ua->sa_mask;
    }
    return 0;
}

int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t, uint64_t,
                           uint64_t) {
    Task* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return -22;
    }
    if (oset != 0) {
        *reinterpret_cast<SigSet*>(oset) = task->sig_blocked;
    }
    if (set != 0) {
        SigSet newset = *reinterpret_cast<const SigSet*>(set);
        // SIGKILL/SIGSTOP cannot be blocked.
        newset &= ~(cinux::proc::sig_make_mask(Signal::kSigkill) |
                    cinux::proc::sig_make_mask(Signal::kSigstop));
        switch (how) {
        case 0:
            task->sig_blocked |= newset;
            break;  // SIG_BLOCK
        case 1:
            task->sig_blocked &= ~newset;
            break;  // SIG_UNBLOCK
        case 2:
            task->sig_blocked = newset;
            break;  // SIG_SETMASK
        default:
            return -22;  // EINVAL
        }
    }
    return 0;
}

}  // namespace cinux::syscall
