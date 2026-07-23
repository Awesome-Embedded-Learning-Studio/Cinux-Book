/**
 * @file kernel/proc/signal.cpp
 * @brief Signal disposition lookup, task registry, delivery, and frames (F3-M1)
 *
 * Batch 1: default-disposition table + uncatchable predicate.
 * Batch 2: pid->Task registry, signal_send, deliverable selection, default
 *          action execution, syscall-path check-and-deliver.
 * Batch 3: custom-handler delivery on the interrupt return path (builds a
 *          signal frame on the user stack + an int $0x80 trampoline) and
 *          sigreturn to restore the saved context.
 *
 * Namespace: cinux::proc
 */

#include "kernel/proc/signal.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/user_access.hpp"  // P3 (SMAP): stac/clac for frame write
#include "kernel/arch/x86_64/usermode.hpp"     // F9 batch 1: USER_SIGRETURN_PAGE
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"  // DEBT-008: VMA check in signal_setup_frame
#include "kernel/mm/vma.hpp"            // VmaFlags / VMA / has_flag
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"  // Spinlock (F-QA Q4c-2 / DEBT-001 registry lock)

namespace cinux::proc {

using cinux::arch::InterruptFrame;

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
    // Returning the pointer after release is safe only while tasks are never
    // freed (DEBT-002 not yet fixed); Q4e must revisit (RCU-safe or re-lock).
    for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
        if (t->pid == pid) {
            return t;
        }
    }
    return nullptr;
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

int signal_send(Task* target, Signal sig) {
    if (!signal_valid(static_cast<int>(sig))) {
        return -22;  // EINVAL
    }
    if (target == nullptr || target->state == TaskState::Zombie ||
        target->state == TaskState::Dead) {
        return -3;  // ESRCH
    }
    // A signal with disposition SIG_IGN is discarded unless it is uncatchable
    // (SIGKILL/SIGSTOP), which override SIG_IGN.
    if (!signal_is_uncatchable(sig) &&
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
    // TODO(future): wake a Blocked target for SIGKILL/SIGTERM (interruptible
    // sleep).  Stopped targets are resumed above; Blocked waits (futex/waitpid)
    // remain non-interruptible until that work lands.
    return 0;
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

int signal_pick_deliverable(Task* task, bool allow_custom) {
    if (task == nullptr) {
        return 0;
    }
    const SigSet avail = task->sig_pending & ~task->sig_blocked;
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
        task->exit_status = static_cast<int>(sig);
        cinux::lib::kprintf("[SIGNAL] default kill: tid=%u '%s' by SIG%d\n",
                            static_cast<unsigned>(task->tid), task->name, static_cast<int>(sig));
        Scheduler::exit_current();  // does not return
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
    Signal           sig = static_cast<Signal>(n);
    const SigAction& act = task->sig_actions->actions[n];
    switch (act.type) {
    case HandlerType::kDefault:
        signal_exec_default(task, sig);  // may not return (terminate)
        break;
    case HandlerType::kIgnore:
        break;
    case HandlerType::kCustom:
        // Left pending; delivered on the interrupt return path (batch 3).
        break;
    }
}

// ============================================================
// Custom-handler delivery + sigreturn (batch 3)
// ============================================================

void signal_setup_frame(InterruptFrame* frame, Signal sig, uint64_t handler_addr, SigSet sa_mask) {
    (void)sa_mask;  // TODO: block sa_mask (+ sig) during the handler
    const uint64_t user_rsp = frame->rsp;
    // Align so the handler entry RSP satisfies the SysV AMD64 ABI (RSP%16==8).
    const uint64_t pad      = user_rsp & 0x0F;  // 0 or 8

    // Layout (low -> high address):
    //   return-addr slot (= USER_SIGRETURN_PAGE)  @ R    <- handler RSP
    //   SignalFrame                               @ R+8  <- sigreturn reads here
    //   (alignment pad)                           @ R+8+sizeof(SignalFrame)
    //   original user RSP                         @ U
    //
    // F9 batch 1: the handler return address points at the fixed
    // USER_SIGRETURN_PAGE (mapped by execve), not at code written on the stack.
    // This keeps the user stack NX-able (F9 batch 2): the 8-byte int $0x80
    // sigreturn stub lives off-stack, aligned with Linux's vDSO __restore_rt.
    const uint64_t R = user_rsp - pad - sizeof(SignalFrame);

    // DEBT-008: before writing the frame + trampoline, validate that [T, user_rsp)
    // lands in a writable Stack VMA.  A signal received with the stack near its
    // limit (or below the guard page) would otherwise fault mid-delivery --
    // frame written half-way, original signal in-flight, stack corrupted -> hang.
    // Fall back to the default action (typically terminate).  Runs at IF=0 (from
    // signal_check_deliver_isr in the ISR path), so irq_guard is a no-op lock;
    // it only documents the critical section.
    Task* task = Scheduler::current();
    if (task != nullptr && task->addr_space != nullptr) {
        auto            vma_guard = task->addr_space->vma_lock().irq_guard();
        cinux::mm::VMA* v         = task->addr_space->vmas().find(R);
        const bool writable_stack = v != nullptr &&
                                    cinux::mm::has_flag(v->flags, cinux::mm::VmaFlags::Write) &&
                                    cinux::mm::has_flag(v->flags, cinux::mm::VmaFlags::Stack);
        if (!writable_stack) {
            cinux::lib::kprintf(
                "[SIGNAL] handler frame R=0x%lx outside writable Stack VMA; "
                "falling back to default action\n",
                static_cast<unsigned long>(R));
            signal_exec_default(task, sig);  // may not return (terminate)
            return;
        }
    }

    // P3 (SMAP real): the SignalFrame + return-addr slots live in user memory;
    // wrap the writes in a local stac/clac window (global STAC removed). Skip
    // access_ok: R was just validated against the writable Stack VMA above.
    cinux::arch::stac();
    auto* sf = reinterpret_cast<SignalFrame*>(R + 8);

    // Save the interrupted user context.
    sf->r15    = frame->r15;
    sf->r14    = frame->r14;
    sf->r13    = frame->r13;
    sf->r12    = frame->r12;
    sf->r11    = frame->r11;
    sf->r10    = frame->r10;
    sf->r9     = frame->r9;
    sf->r8     = frame->r8;
    sf->rdi    = frame->rdi;
    sf->rsi    = frame->rsi;
    sf->rbp    = frame->rbp;
    sf->rdx    = frame->rdx;
    sf->rcx    = frame->rcx;
    sf->rbx    = frame->rbx;
    sf->rax    = frame->rax;
    sf->rip    = frame->rip;
    sf->rflags = frame->rflags;
    sf->rsp    = user_rsp;
    sf->sig    = static_cast<uint64_t>(sig);
    sf->magic  = kSigFrameMagic;

    // Return address: handler `ret` lands on the fixed sigreturn page (not
    // stack-resident code).  See USER_SIGRETURN_PAGE / kSigreturnTrampoline.
    *reinterpret_cast<uint64_t*>(R) = cinux::arch::USER_SIGRETURN_PAGE;
    cinux::arch::clac();  // end user-frame write window

    // Redirect the interrupted frame to enter the handler with sig as %rdi.
    frame->rip = handler_addr;
    frame->rsp = R;
    frame->rdi = static_cast<uint64_t>(sig);
    frame->rax = 0;
}

extern "C" void signal_check_deliver_isr(InterruptFrame* frame) {
    Task* task = Scheduler::current();
    if (task == nullptr) {
        return;
    }
    // Only deliver when returning to user mode; a signal that arrives while
    // the kernel is running is deferred to the next user-mode return.
    if ((frame->cs & 0x03) == 0) {
        return;
    }
    int n = signal_pick_deliverable(task, /*allow_custom=*/true);
    if (n == 0) {
        return;
    }
    Signal           sig = static_cast<Signal>(n);
    const SigAction& act = task->sig_actions->actions[n];
    switch (act.type) {
    case HandlerType::kDefault:
        signal_exec_default(task, sig);  // may not return (terminate)
        break;
    case HandlerType::kIgnore:
        break;
    case HandlerType::kCustom:
        signal_setup_frame(frame, sig, act.handler_addr, act.sa_mask);
        break;
    }
}

extern "C" void sigreturn_handler(InterruptFrame* frame) {
    // The handler returned into the int $0x80 trampoline.  user RSP points
    // just past the return-address slot, i.e. at the saved SignalFrame.
    auto* sf = reinterpret_cast<SignalFrame*>(frame->rsp);
    if (sf->magic != kSigFrameMagic) {
        cinux::lib::kprintf("[SIGNAL] sigreturn: bad frame magic %p -- killing task\n",
                            reinterpret_cast<void*>(sf->magic));
        if (Task* task = Scheduler::current(); task != nullptr) {
            task->exit_status = static_cast<int>(Signal::kSigkill);
            Scheduler::exit_current();  // does not return
        }
        return;
    }
    // Restore the interrupted user context into the frame; the ISR stub will
    // pop the GPRs and IRETQ using these values.
    frame->r15    = sf->r15;
    frame->r14    = sf->r14;
    frame->r13    = sf->r13;
    frame->r12    = sf->r12;
    frame->r11    = sf->r11;
    frame->r10    = sf->r10;
    frame->r9     = sf->r9;
    frame->r8     = sf->r8;
    frame->rdi    = sf->rdi;
    frame->rsi    = sf->rsi;
    frame->rbp    = sf->rbp;
    frame->rdx    = sf->rdx;
    frame->rcx    = sf->rcx;
    frame->rbx    = sf->rbx;
    frame->rax    = sf->rax;
    frame->rip    = sf->rip;
    frame->rflags = sf->rflags;
    frame->rsp    = sf->rsp;
}

}  // namespace cinux::proc
