/**
 * @file kernel/syscall/sys_signal.cpp
 * @brief Signal syscall handlers (F3-M1 batch 2 / P0d SMAP-layered)
 *
 * Layered (Linux-aligned):
 *   - do_kill_kernel / do_sigaction_kernel / do_sigprocmask_kernel: pure
 *     kernel-to-kernel signal logic (kernel SigAction / SigSet), no user memory.
 *     Tests call these with kernel structs.
 *   - sys_kill / sys_rt_sigaction / sys_rt_sigprocmask: thin user boundaries.
 *     The user/kernel sigaction translation seam (user sa_handler 0/1/addr <->
 *     kernel HandlerType; sa_flags bits <-> bools) lives HERE; UserSigAction is
 *     staged in/out via copy_from/to_user (32B whole-frame, never per-field).
 *     SigSet is get_user/put_user (8B).
 *
 * The raw dereferences of the user act/oact/set/oset structs are gone -- the
 * old "kernel maps the caller's AS, bad pointer faults" comment (see header)
 * assumed global STAC, which P3 removes.
 */

#include "kernel/syscall/sys_signal.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0d (SMAP): copy_to/from_user, get/put_user
#include "kernel/errno.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/proc/sync.hpp"  // B3b: InterruptGuard (rt_sigtimedwait atomic park)

namespace cinux::syscall {

using cinux::proc::HandlerType;
using cinux::proc::Signal;
using cinux::proc::SigAction;
using cinux::proc::SigSet;
using cinux::proc::Task;

// Linux sigaction flags (uapi). SA_RESTORER (0x04000000) not honoured: CinuxOS
// injects its own sigreturn trampoline rather than the user restorer.
constexpr uint64_t kSaRestart   = 0x10000000;
constexpr uint64_t kSaResethand = 0x80000000;

namespace {

void user_to_kernel_sigaction(const UserSigAction& u, SigAction& k) {
    if (u.sa_handler == 0) {
        k.type = HandlerType::kDefault;
    } else if (u.sa_handler == 1) {
        k.type = HandlerType::kIgnore;
    } else {
        k.type         = HandlerType::kCustom;
        k.handler_addr = u.sa_handler;
    }
    k.sa_restart   = (u.sa_flags & kSaRestart) != 0;
    k.sa_resethand = (u.sa_flags & kSaResethand) != 0;
    k.sa_mask      = u.sa_mask;
}

void kernel_to_user_sigaction(const SigAction& k, UserSigAction& u) {
    u.sa_handler  = (k.type == HandlerType::kIgnore)   ? 1
                    : (k.type == HandlerType::kCustom) ? k.handler_addr
                                                       : 0;
    u.sa_flags    = (k.sa_restart ? kSaRestart : 0) | (k.sa_resethand ? kSaResethand : 0);
    u.sa_restorer = 0;  // CinuxOS injects its own sigreturn trampoline
    u.sa_mask     = k.sa_mask;
}

}  // anonymous namespace

// ============================================================
// do_*_kernel: pure kernel-to-kernel signal logic (no user memory)
// ============================================================

int64_t do_kill_kernel(Task* cur, int32_t pid, int sig) {
    if (!cinux::proc::signal_valid(sig)) {
        return -cinux::kEinval;
    }
    Signal s      = static_cast<Signal>(sig);
    Task*  target = nullptr;
    if (pid == 0 || (cur != nullptr && pid == cur->pid)) {
        target = cur;  // signal self
    } else if (pid > 0) {
        target = cinux::proc::signal_find_task_by_pid(pid);
    } else {
        // POSIX kill(-pgid, sig): signal the whole process group (F3-M3).
        int sent = cinux::proc::killpg(-pid, s);
        if (sent == 0) {
            return -cinux::kEsrch;  // no such process group
        }
        return 0;
    }
    if (target == nullptr) {
        return -cinux::kEsrch;
    }
    return cinux::proc::signal_send(target, s);
}

int64_t do_sigaction_kernel(Task* task, int sig, const SigAction* new_act, SigAction* old_act) {
    if (!cinux::proc::signal_valid(sig) || task == nullptr) {
        return -cinux::kEinval;
    }
    Signal s = static_cast<Signal>(sig);
    if (cinux::proc::signal_is_uncatchable(s)) {  // SIGKILL/SIGSTOP cannot be caught
        return -cinux::kEinval;
    }
    SigAction& slot = task->sig_actions->actions[sig];
    if (old_act != nullptr) {
        *old_act = slot;  // POSIX: old before new (in-place act==oact is safe)
    }
    if (new_act != nullptr) {
        slot = *new_act;
    }
    return 0;
}

int64_t do_sigprocmask_kernel(Task* task, int how, const SigSet* set, SigSet* oset) {
    if (task == nullptr) {
        return -cinux::kEinval;
    }
    if (oset != nullptr) {
        *oset = task->sig_blocked;
    }
    if (set != nullptr) {
        SigSet newset = *set;
        newset &= ~(cinux::proc::sig_make_mask(Signal::kSigkill) |
                    cinux::proc::sig_make_mask(Signal::kSigstop));  // unblockable
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
            return -cinux::kEinval;
        }
    }
    return 0;
}

// ============================================================
// sys_* boundaries: accessor stages user structs -> do_*_kernel
// ============================================================

int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t) {
    return do_kill_kernel(cinux::proc::Scheduler::current(), static_cast<int32_t>(pid),
                          static_cast<int>(sig));
}

int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t, uint64_t, uint64_t) {
    Task* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return -cinux::kEinval;
    }
    int n = static_cast<int>(sig);

    SigAction        new_k;
    const SigAction* new_p = nullptr;
    if (act != 0) {
        UserSigAction u;
        if (!cinux::user::copy_from_user(&u, reinterpret_cast<const void*>(act), sizeof(u))) {
            return -cinux::kEfault;
        }
        user_to_kernel_sigaction(u, new_k);
        new_p = &new_k;
    }

    SigAction  old_k;
    SigAction* old_p = (oact != 0) ? &old_k : nullptr;
    int64_t    rc    = do_sigaction_kernel(task, n, new_p, old_p);
    if (rc < 0) {
        return rc;
    }

    if (oact != 0) {
        UserSigAction uo;
        kernel_to_user_sigaction(old_k, uo);
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(oact), &uo, sizeof(uo))) {
            return -cinux::kEfault;
        }
    }
    return 0;
}

int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t, uint64_t,
                           uint64_t) {
    Task* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return -cinux::kEinval;
    }

    SigSet        kset;
    const SigSet* set_p = nullptr;
    if (set != 0) {
        if (!cinux::user::get_user(&kset, reinterpret_cast<const SigSet*>(set))) {
            return -cinux::kEfault;
        }
        set_p = &kset;
    }

    SigSet  koset;
    SigSet* oset_p = (oset != 0) ? &koset : nullptr;
    int64_t rc     = do_sigprocmask_kernel(task, static_cast<int>(how), set_p, oset_p);
    if (rc < 0) {
        return rc;
    }

    if (oset != 0) {
        if (!cinux::user::put_user(koset, reinterpret_cast<SigSet*>(oset))) {
            return -cinux::kEfault;
        }
    }
    return 0;
}

int64_t sys_rt_sigtimedwait(uint64_t set, uint64_t info, uint64_t /*timeout*/, uint64_t, uint64_t,
                            uint64_t) {
    Task* task = cinux::proc::Scheduler::current();
    if (task == nullptr || set == 0) {
        return -cinux::kEfault;
    }
    SigSet wait_set;
    if (!cinux::user::get_user(&wait_set, reinterpret_cast<const SigSet*>(set))) {
        return -cinux::kEfault;
    }

    // B3b: busybox init's main loop polls SIGCHLD here.  Return -EAGAIN when no
    // matching signal is pending -- init treats it as a timeout and loops to
    // check respawn actions (fork /bin/sh) and reap children.  busybox init
    // drives respawn from sigtimedwait's return, so a pure block (that only a
    // signal can wake) deadlocks when no child has been forked yet.  The
    // round-robin scheduler's timeslice keeps this loop from starving sh.  The
    // opt-in Task::sigwait_blocked wake in signal_send() stays wired for a
    // future blocking+timeout variant.
    cinux::proc::InterruptGuard guard;
    (void)guard;
    SigSet avail = task->sig_pending & wait_set;
    if (avail != 0) {
        for (int n = 1; n <= cinux::proc::kSignalMax; ++n) {
            if (avail & (SigSet{1} << n)) {
                task->sig_pending &= ~(SigSet{1} << n);
                if (info != 0) {
                    // siginfo_t (~128B) zeroed -- busybox init ignores it.
                    uint64_t zero[16] = {};
                    cinux::user::copy_to_user(reinterpret_cast<void*>(info), zero, sizeof(zero));
                }
                return n;
            }
        }
    }
    return -cinux::kEagain;
}

}  // namespace cinux::syscall
