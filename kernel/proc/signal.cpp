/**
 * @file kernel/proc/signal.cpp
 * @brief Signal disposition lookup, task registry, and default delivery
 *
 * Batch 1: default-disposition table + uncatchable predicate.
 * Batch 2: pid->Task registry, signal_send, deliverable selection, default
 *          action execution, syscall-path check-and-deliver.
 * Batch 3 signal-frame setup and sigreturn live in signal_frame.cpp.
 *
 * Namespace: cinux::proc
 */

#include "kernel/proc/signal.hpp"

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"           // Spinlock (F-QA Q4c-2 / DEBT-001 registry lock)
#include "kernel/proc/task_snapshot.hpp"  // TaskSnapshot (DEBT-022)
#include "kernel/syscall/sys_exit.hpp"  // F-ECO: default-kill routes through sys_exit (Zombie+notify)

namespace cinux::proc {

namespace {

// ============================================================
// Task registry: a simple singly-linked list keyed by pid, so sys_kill can
// resolve a target Task*.  Tasks register in Scheduler::add_task and leave
// in remove_task / exit_current.
// ============================================================

Task* g_registry_head = nullptr;

// F-QA Q4c-2 (DEBT-001): the registry is a global mutable list touched by
// add_task (register), exit (unregister), sys_kill (find), and killpg (walk).
// All four may run on different CPUs; an unlocked head-insert racing a walk
// reads half-linked pointers / dangling registry_next -> jump-to-garbage or
// UAF once the slab reuses a freed Task. irq-safe: add_task may run at IF=0.
Spinlock g_registry_lock;

}  // namespace

void signal_register_task(Task* task) {
    if (task == nullptr) {
        return;
    }
    auto g              = g_registry_lock.irq_guard();  // DEBT-001
    task->registry_next = g_registry_head;
    g_registry_head     = task;
}

void signal_unregister_task(Task* task) {
    if (task == nullptr) {
        return;
    }
    auto   g  = g_registry_lock.irq_guard();  // DEBT-001
    Task** pp = &g_registry_head;
    while (*pp != nullptr) {
        if (*pp == task) {
            *pp                 = task->registry_next;
            task->registry_next = nullptr;
            return;
        }
        pp = &(*pp)->registry_next;
    }
}

Task* signal_find_task_by_pid(int pid) {
    auto g = g_registry_lock.irq_guard();  // DEBT-001
    // WARNING: @p g releases on return, so the returned Task* is only valid
    // while the lock is held.  Tasks ARE freed now (DEBT-002 fixed in F-QA
    // Q4e-3: exit_current -> reap_deferred -> delete), so a caller that
    // dereferences this pointer after this function returns races with
    // free-on-another-CPU -> use-after-free.  Callers that only need fields
    // must use signal_snapshot_task() below (DEBT-022), which copies under the
    // lock.  Remaining raw-pointer callers (tests, sys_pgrp, /proc lookup
    // liveness gates) only test for nullptr or tolerate Zombie/Dead.
    for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
        if (t->pid == pid) {
            return t;
        }
    }
    return nullptr;
}

bool signal_snapshot_task(int pid, TaskSnapshot& out) {
    auto g = g_registry_lock.irq_guard();  // DEBT-001
    for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
        if (t->pid == pid) {
            out.pid    = t->pid;
            out.state  = t->state;
            out.ppid   = t->ppid;
            out.tgid   = t->tgid;
            out.uid    = t->uid;
            out.gid    = t->gid;
            // Copy name BYTES, not the pointer: the snapshot must survive the
            // lock releasing (and, defensively, any future change to name
            // storage).  Truncate at kTaskNameMax-1 like Linux TASK_COMM_LEN.
            uint32_t i = 0;
            if (t->name != nullptr) {
                while (i + 1 < kTaskNameMax && t->name[i] != '\0') {
                    out.name[i] = t->name[i];
                    ++i;
                }
            }
            out.name[i] = '\0';
            return true;
        }
    }
    return false;
}

bool signal_nth_task_pid(uint32_t n, int* out_pid) {
    if (out_pid == nullptr) {
        return false;
    }
    // Walk under the registry lock (DEBT-001) to the n-th task.  readdir calls
    // this per index, so a full /proc listing is O(tasks^2); tasks are bounded
    // by PID_MAX (256), so this is negligible.  A task added/removed between two
    // readdir indices can shift the n-th entry -- the classic readdir-under-
    // mutation caveat (Linux uses seq positions); the registry is normally stable
    // across one listing, and lookup re-checks liveness regardless.
    auto     g = g_registry_lock.irq_guard();
    uint32_t i = 0;
    for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
        if (i == n) {
            *out_pid = t->pid;
            return true;
        }
        ++i;
    }
    return false;
}

// ============================================================
// Default disposition table (batch 1)
// ============================================================

// POSIX default dispositions.  Unlisted signals default to Terminate.
SigDefault signal_default_action(Signal sig) {
    switch (sig) {
    case Signal::kSigquit:
    case Signal::kSigill:
    case Signal::kSigtrap:
    case Signal::kSigabrt:
    case Signal::kSigbus:
    case Signal::kSigfpe:
    case Signal::kSigsegv:
        return SigDefault::kCoreDump;
    case Signal::kSigchld:
        return SigDefault::kIgnore;
    case Signal::kSigcont:
        return SigDefault::kContinue;
    case Signal::kSigstop:
    case Signal::kSigtstp:
    case Signal::kSigttin:
    case Signal::kSigtou:
        return SigDefault::kStop;
    default:
        return SigDefault::kTerminate;
    }
}

bool signal_is_uncatchable(Signal sig) {
    return sig == Signal::kSigkill || sig == Signal::kSigstop;
}

// ============================================================
// Delivery (batch 2)
// ============================================================

int queue_signal(Task* target, Signal sig, bool force) {
    if (!signal_valid(static_cast<int>(sig))) {
        return -22;  // EINVAL
    }
    if (target == nullptr || target->state == TaskState::Zombie ||
        target->state == TaskState::Dead) {
        return -3;  // ESRCH
    }
    if (force) {
        // SMP-safe force: tag the signal per-task so delivery bypasses the
        // block mask and SIG_IGN.  Do NOT mutate sig_blocked or the shared
        // SharedSigActions table -- both are read locklessly by other CPUs and
        // CLONE_SIGHAND siblings, so rewriting them (a) races those readers and
        // (b) permanently corrupts user disposition across the group.  Mirrors
        // Linux force_sig_info: force is a delivery-time behaviour, not state.
        sig_set_add(target->sig_forced, sig);
    }
    // A signal with disposition SIG_IGN is discarded unless it is uncatchable
    // (SIGKILL/SIGSTOP), which override SIG_IGN.
    if (!force && !signal_is_uncatchable(sig) &&
        target->sig_actions->actions[static_cast<int>(sig)].type == HandlerType::kIgnore) {
        return 0;
    }
    // A Stopped task is never scheduled, so it cannot deliver a signal to
    // itself.  SIGCONT (resume) and SIGKILL (must terminate) therefore pull a
    // stopped task out of the stopped state at send time.  POSIX also has
    // SIGCONT discard any pending stop signals.
    if (sig == Signal::kSigcont) {
        const SigSet stop_mask = sig_make_mask(Signal::kSigstop) | sig_make_mask(Signal::kSigtstp) |
                                 sig_make_mask(Signal::kSigttin) | sig_make_mask(Signal::kSigtou);
        target->sig_pending &= ~stop_mask;
    }
    if (target->state == TaskState::Stopped &&
        (sig == Signal::kSigcont || sig == Signal::kSigkill)) {
        target->state = TaskState::Ready;
        if (target->sched_class != nullptr) {
            target->sched_class->enqueue(target);
        }
    }
    sig_set_add(target->sig_pending, sig);
    // B3b (busybox init): wake a task blocked in rt_sigtimedwait waiting for a
    // matching signal.  Precise and opt-in (only sigwait_blocked targets), so
    // futex/waitpid Blocked waits stay non-interruptible until the broader
    // "interruptible sleep" TODO above lands.
    if (target->sigwait_blocked) {
        target->sigwait_blocked = false;
        Scheduler::unblock(target);
    }
    // EINTR: a task parked in a blocking syscall (pipe/socket/poll) recorded
    // its wait-queue head in wait_queue_head.  Wake it so its blocking loop
    // resumes, sees signal_deliverable_pending(), and returns -EINTR.  Only
    // fires when the signal is actually deliverable right now (unblocked or
    // forced) -- a blocked-but-pending signal (e.g. SIGINT while sigprocmask
    // hides it) must NOT spuriously kick a sleeper out (it would busy-loop).
    // Scheduler::unblock is idempotent: if a fd/timer already woke it, this is
    // a no-op and the loop's normal data-ready path still wins.  Mutex/
    // Semaphore/futex/waitpid sleeps leave wait_queue_head == nullptr and so
    // are never interrupted here (matches Linux: kernel mutexes are
    // TASK_UNINTERRUPTIBLE).
    if (target->wait_queue_head != nullptr && target->state == TaskState::Blocked &&
        signal_deliverable_pending(target)) {
        Scheduler::unblock(target);
    }
    return 0;
}

int signal_send(Task* target, Signal sig) {
    return queue_signal(target, sig, /*force=*/false);
}

int signal_force_send(Task* target, Signal sig) {
    return queue_signal(target, sig, /*force=*/true);
}

bool signal_deliverable_pending(const Task* task) {
    // EINTR check for blocking IO loops: does @p task have ANY signal that
    // would be delivered if it returned to user mode right now?  Mirrors the
    // avail-mask of signal_pick_deliverable() (unblocked OR forced) but peeks
    // WITHOUT consuming -- a wake followed by -EINTR only makes sense if the
    // signal will actually fire on the return-to-user path.
    if (task == nullptr) {
        return false;
    }
    const SigSet avail = (task->sig_pending & ~task->sig_blocked) | task->sig_forced;
    return avail != 0;
}

int killpg(int pgid, Signal sig) {
    if (!signal_valid(static_cast<int>(sig))) {
        return -22;  // EINVAL
    }
    if (pgid == 0) {
        Task* cur = Scheduler::current();
        if (cur == nullptr) {
            return -3;  // ESRCH: no current task to resolve "own group"
        }
        pgid = cur->pgid;
    }
    // F-QA Q4c-2 (DEBT-001): snapshot matching targets under the registry lock,
    // then signal_send() AFTER releasing it. signal_send() may terminate the
    // target (-> exit_current -> schedule); holding the lock across that would
    // trip lockdep / deadlock. Targets stay valid while tasks are never freed
    // (DEBT-002 not yet fixed); Q4e must revisit.
    constexpr int kMaxKillTargets = 64;  // real process groups are far smaller
    Task*         targets[kMaxKillTargets];
    int           ntargets = 0;
    {
        auto g = g_registry_lock.irq_guard();
        for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
            if (t->pgid == pgid && ntargets < kMaxKillTargets) {
                targets[ntargets++] = t;
            }
        }
    }
    int sent = 0;
    for (int i = 0; i < ntargets; ++i) {
        signal_send(targets[i], sig);  // tolerates Zombie/Dead (returns ESRCH)
        ++sent;
    }
    return sent;
}

void itimer_real_tick(uint64_t delta_ns) {
    // Walk every task under the registry lock, decrement its ITIMER_REAL, and
    // collect those that expired.  signal_send() runs AFTER releasing the lock:
    // it may wake/terminate the target (queue_signal -> unblock / exit_current),
    // none of which needs the registry lock, and holding it across that would
    // risk lockdep/deadlock.  The PIT IRQ is non-reentrant (irq0, EOI after),
    // so this runs once per tick; cross-CPU setitimer races the fields but
    // aligned 64-bit rw are atomic and a missed/repeated SIGALRM is benign.
    constexpr int kMaxExpired = 64;  // matches killpg's cap; rarely >1 timer armed
    Task*         expired[kMaxExpired];
    int           nexpired = 0;
    {
        auto g = g_registry_lock.irq_guard();
        for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
            if (t->itimer_real_value_ns == 0) {
                continue;  // disarmed
            }
            if (t->itimer_real_value_ns > delta_ns) {
                t->itimer_real_value_ns -= delta_ns;
            } else {
                // Expired: reload from interval (0 = one-shot -> disarms) and
                // queue SIGALRM.  Reload BEFORE signalling so a periodic timer
                // keeps ticking even while the handler is pending.
                t->itimer_real_value_ns = t->itimer_real_interval_ns;
                if (nexpired < kMaxExpired) {
                    expired[nexpired++] = t;
                }
            }
        }
    }
    for (int i = 0; i < nexpired; ++i) {
        signal_send(expired[i], Signal::kSigalrm);
    }
}

int signal_pick_deliverable(Task* task, bool allow_custom) {
    if (task == nullptr) {
        return 0;
    }
    // Forced signals (sync faults) bypass the block mask; the force tag is
    // consumed (cleared) in the delivery paths below.
    const SigSet avail = (task->sig_pending & ~task->sig_blocked) | task->sig_forced;
    for (int n = 1; n <= kSignalMax; n++) {
        if ((avail & (SigSet{1} << n)) == 0) {
            continue;
        }
        // Custom handlers need a signal frame.  The syscall return path
        // (allow_custom=false) cannot build one, so it skips them; the
        // interrupt path (allow_custom=true) delivers them.
        if (!allow_custom && task->sig_actions->actions[n].type == HandlerType::kCustom) {
            continue;
        }
        task->sig_pending &= ~(SigSet{1} << n);
        return n;
    }
    return 0;
}

void signal_exec_default(Task* task, Signal sig) {
    switch (signal_default_action(sig)) {
    case SigDefault::kTerminate:
    case SigDefault::kCoreDump:
        // F-ECO batch 0: a signal-default-killed task must become a Zombie the
        // parent can waitpid-reap (WIFSIGNALED), NOT vanish via exit_current()'s
        // Dead+deferred-free path -- that orphaned the child so waitpid spun
        // forever (busybox SIGILL hit this on echo iter 1). Route through
        // sys_exit(): sets exit_status=sig, sends SIGCHLD, flips state to Zombie,
        // dequeues, unblocks a wait()ing parent, then yields (does not return).
        // Same path as a normal user exit(). (sigreturn_handler's bad-frame kill
        // further down is the same class -- left as follow-up, off the smoke path.)
        cinux::lib::kprintf("[SIGNAL] default kill: tid=%u '%s' by SIG%d\n",
                            static_cast<unsigned>(task->tid), task->name, static_cast<int>(sig));
        cinux::syscall::exit_and_reap_current(static_cast<int>(sig));  // WIFSIGNALED (sig in low byte); does not return
        break;
    case SigDefault::kIgnore:
        break;
    case SigDefault::kStop:
        // Remove the task from scheduling.  Reached on the target's own delivery
        // path while it is running; if it is current, switch away (resumes here
        // if it is later continued).
        task->state = TaskState::Stopped;
        if (task->sched_class != nullptr) {
            task->sched_class->dequeue(task);
        }
        cinux::lib::kprintf("[SIGNAL] default stop: tid=%u '%s' by SIG%d\n",
                            static_cast<unsigned>(task->tid), task->name, static_cast<int>(sig));
        if (task == Scheduler::current()) {
            Scheduler::schedule();
        }
        break;
    case SigDefault::kContinue:
        // Resume a stopped task.  A task that is already running/ready treats
        // SIGCONT as a no-op.  Normally a stopped task is resumed at send time
        // (signal_send); this path covers the rare case of a Continue signal
        // delivered while the task is still runnable.
        if (task->state == TaskState::Stopped) {
            task->state = TaskState::Ready;
            if (task->sched_class != nullptr) {
                task->sched_class->enqueue(task);
            }
        }
        break;
    }
}

void signal_check_and_deliver() {
    Task* task = Scheduler::current();
    if (task == nullptr) {
        return;
    }
    int n = signal_pick_deliverable(task, /*allow_custom=*/false);
    if (n == 0) {
        return;
    }
    Signal     sig    = static_cast<Signal>(n);
    const bool forced = (task->sig_forced & (SigSet{1} << n)) != 0;
    task->sig_forced &= ~(SigSet{1} << n);  // consume the force tag
    const SigAction& act = task->sig_actions->actions[n];
    switch (act.type) {
    case HandlerType::kDefault:
        signal_exec_default(task, sig);  // may not return (terminate)
        break;
    case HandlerType::kIgnore:
        // A forced signal bypasses SIG_IGN and runs the default action so a
        // sync fault cannot livelock by returning to the faulting RIP.
        if (forced) {
            signal_exec_default(task, sig);
        }
        break;
    case HandlerType::kCustom:
        // Left pending; delivered on the interrupt return path (batch 3).
        break;
    }
}

}  // namespace cinux::proc
