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
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

using cinux::arch::InterruptFrame;

namespace {

// ============================================================
// Task registry: a simple singly-linked list keyed by pid, so sys_kill can
// resolve a target Task*.  Tasks register in Scheduler::add_task and leave
// in remove_task / exit_current.
// ============================================================

Task* g_registry_head = nullptr;

// int $0x80 (opcode cd 80) followed by nops to fill an 8-byte slot.  This is
// the sigreturn trampoline written onto the user stack; the handler's return
// address points here, so `ret` lands on `int $0x80` which traps into the
// sigreturn IDT gate (vector 0x80).  Requires the user stack to be executable
// (NXE is off until F9 -- see GOTCHA).
constexpr uint8_t kSigreturnTrampoline[8] = {0xCD, 0x80, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

}  // namespace

void signal_register_task(Task* task) {
    if (task == nullptr) {
        return;
    }
    task->registry_next = g_registry_head;
    g_registry_head     = task;
}

void signal_unregister_task(Task* task) {
    if (task == nullptr) {
        return;
    }
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
        target->sig_actions[static_cast<int>(sig)].type == HandlerType::kIgnore) {
        return 0;
    }
    sig_set_add(target->sig_pending, sig);
    // TODO(batch later): wake a Blocked target for SIGKILL/SIGCONT/SIGTERM.
    return 0;
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
        if (!allow_custom && task->sig_actions[n].type == HandlerType::kCustom) {
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
    case SigDefault::kContinue:
        // TODO(batch later): job-control stop/cont need a Stopped task state
        // and scheduler exclusion.  No-op for now.
        cinux::lib::kprintf("[SIGNAL] stop/cont not yet supported (SIG%d)\n",
                            static_cast<int>(sig));
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
    const SigAction& act = task->sig_actions[n];
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
    //   trampoline code (8B)        @ T
    //   return-addr slot (= T)      @ R        <- handler RSP
    //   SignalFrame                 @ R+8      <- sigreturn reads here
    //   (alignment pad)             @ R+8+sizeof(SignalFrame)
    //   original user RSP           @ U
    const uint64_t R  = user_rsp - pad - 8 - sizeof(SignalFrame);
    const uint64_t T  = R - 8;
    auto*          sf = reinterpret_cast<SignalFrame*>(R + 8);

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

    // Trampoline code + return address pointing at it.
    auto* tramp = reinterpret_cast<uint8_t*>(T);
    for (int i = 0; i < 8; i++) {
        tramp[i] = kSigreturnTrampoline[i];
    }
    *reinterpret_cast<uint64_t*>(R) = T;

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
    const SigAction& act = task->sig_actions[n];
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
