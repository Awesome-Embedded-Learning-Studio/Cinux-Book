/**
 * @file kernel/proc/signal_frame.cpp
 * @brief Signal-frame construction, interrupt-path delivery, and sigreturn
 */

#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/user_access.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/syscall/sys_exit.hpp"

namespace cinux::proc {

using cinux::arch::InterruptFrame;

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
    const uint64_t R = user_rsp - pad - sizeof(SignalFrame);

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

    SignalFrame sf{};
    sf.r15    = frame->r15;
    sf.r14    = frame->r14;
    sf.r13    = frame->r13;
    sf.r12    = frame->r12;
    sf.r11    = frame->r11;
    sf.r10    = frame->r10;
    sf.r9     = frame->r9;
    sf.r8     = frame->r8;
    sf.rdi    = frame->rdi;
    sf.rsi    = frame->rsi;
    sf.rbp    = frame->rbp;
    sf.rdx    = frame->rdx;
    sf.rcx    = frame->rcx;
    sf.rbx    = frame->rbx;
    sf.rax    = frame->rax;
    sf.rip    = frame->rip;
    sf.rflags = frame->rflags;
    sf.rsp    = user_rsp;
    sf.sig    = static_cast<uint64_t>(sig);
    sf.magic  = kSigFrameMagic;

    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(R + 8), &sf, sizeof(sf))) {
        cinux::lib::kprintf("[SIGNAL] handler frame copy failed at 0x%lx\n",
                            static_cast<unsigned long>(R + 8));
        if (task != nullptr) {
            cinux::syscall::sys_exit(static_cast<uint64_t>(Signal::kSigsegv), 0, 0, 0, 0, 0);
        }
        return;
    }

    const uint64_t ret_addr = cinux::arch::USER_SIGRETURN_PAGE;
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(R), &ret_addr, sizeof(ret_addr))) {
        cinux::lib::kprintf("[SIGNAL] handler return slot copy failed at 0x%lx\n",
                            static_cast<unsigned long>(R));
        if (task != nullptr) {
            cinux::syscall::sys_exit(static_cast<uint64_t>(Signal::kSigsegv), 0, 0, 0, 0, 0);
        }
        return;
    }

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

void signal_restore_frame(InterruptFrame* frame, const SignalFrame& sf) {
    frame->r15    = sf.r15;
    frame->r14    = sf.r14;
    frame->r13    = sf.r13;
    frame->r12    = sf.r12;
    frame->r11    = sf.r11;
    frame->r10    = sf.r10;
    frame->r9     = sf.r9;
    frame->r8     = sf.r8;
    frame->rdi    = sf.rdi;
    frame->rsi    = sf.rsi;
    frame->rbp    = sf.rbp;
    frame->rdx    = sf.rdx;
    frame->rcx    = sf.rcx;
    frame->rbx    = sf.rbx;
    frame->rax    = sf.rax;
    frame->rip    = sf.rip;
    frame->rflags = sf.rflags;
    frame->rsp    = sf.rsp;
}

extern "C" void sigreturn_handler(InterruptFrame* frame) {
    SignalFrame sf{};
    if (!cinux::user::copy_from_user(&sf, reinterpret_cast<const void*>(frame->rsp), sizeof(sf))) {
        cinux::lib::kprintf("[SIGNAL] sigreturn: cannot read frame at %p -- killing task\n",
                            reinterpret_cast<void*>(frame->rsp));
        if (Task* task = Scheduler::current(); task != nullptr) {
            cinux::syscall::sys_exit(static_cast<uint64_t>(Signal::kSigkill), 0, 0, 0, 0, 0);
        }
        return;
    }
    if (sf.magic != kSigFrameMagic) {
        cinux::lib::kprintf("[SIGNAL] sigreturn: bad frame magic %p -- killing task\n",
                            reinterpret_cast<void*>(sf.magic));
        if (Task* task = Scheduler::current(); task != nullptr) {
            cinux::syscall::sys_exit(static_cast<uint64_t>(Signal::kSigkill), 0, 0, 0, 0, 0);
        }
        return;
    }
    signal_restore_frame(frame, sf);
}

}  // namespace cinux::proc
