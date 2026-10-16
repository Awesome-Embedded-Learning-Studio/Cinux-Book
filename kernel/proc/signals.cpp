#include "kernel/proc/signals.hpp"

#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::proc {

namespace {

using cinux::syscall::SigAction;
using cinux::syscall::SigSet;
using cinux::syscall::SyscallFrame;
using cinux::syscall::kSignalCount;
using cinux::syscall::kSignalDefault;
using cinux::syscall::kSignalIgnore;

/// The set bit naming one signal number, 1-based.
constexpr SigSet bit_for(int number) {
    return SigSet{1ULL << static_cast<unsigned>(number - 1)};
}

/// Bytes the delivery lays below the interrupted stack pointer: the
/// context, the context-pointer slot and the return slot.
constexpr unsigned long long kDeliveryBytes = sizeof(SigReturnContext) + 16;

/**
 * @brief         Whether the delivery may write its context under rsp.
 * @param[in]     rsp   The interrupted user stack pointer.
 * @return        true when the whole delivery footprint fits inside
 *                the registered stack window.
 * @note          The window check is the overflow answer: a stack so
 *                shallow it cannot hold a signal frame is a task about
 *                to die anyway, and refusing here keeps the kernel from
 *                writing outside its own bookkeeping.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
bool stack_holds_delivery(unsigned long long rsp) {
    return rsp > mm::kUserStackBottom && rsp - kDeliveryBytes >= mm::kUserStackBottom &&
           rsp <= mm::kUserStackTop;
}

/**
 * @brief         The lowest unblocked pending signal, zero when none.
 * @param[in]     state   The task's signal world.
 * @return        The signal number, or 0.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
int lowest_runnable(const SignalState& state) {
    const SigSet kWant = state.pending & ~state.blocked;
    for (int number = 1; number <= kSignalCount; ++number) {
        if (kWant.has(bit_for(number))) {
            return number;
        }
    }
    return 0;
}

/// The four signals whose default disposition is a halt; a continue
/// sweeping these off the pending set is what makes "continue" mean
/// continue rather than "resume into the stop that preceded it".
SigSet stop_class_mask() {
    return bit_for(cinux::syscall::kSigStop) | bit_for(cinux::syscall::kSigTstp) |
           bit_for(cinux::syscall::kSigTtin) | bit_for(cinux::syscall::kSigTtou);
}

/**
 * @brief         Exit the current task killed by an unhandled signal.
 * @param[in]     number   The signal that decided it.
 * @note          The signal number rides out raw as the exit word, so
 *                the parent's wait reads a signal death, not a code.
 *                Only the host test sinks ever see this return.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
void die_by_signal(int number) {
    Scheduler::self().exit_current(static_cast<long long>(number));
}

/**
 * @brief         Build the handler's user stack and point the frame at it.
 * @param[in,out] frame   The frame the task would return on.
 * @param[in]     number   The signal being delivered.
 * @param[in]     act      The installed action.
 * @param[in]     blocked_before   The mask as it stood before delivery.
 * @return        true when the frame was rewritten.
 * @note          The layout under the interrupted rsp, low to high: the
 *                slot holding the context's address, the slot holding
 *                the trampoline's, then the context itself. The frame
 *                then reads like a call just happened: the stack
 *                pointer rests on the trampoline slot (eight mod
 *                sixteen, as the ABI demands after a call), rcx
 *                carries the handler, rdi the signal number. The
 *                handler's ret lands on the trampoline with the
 *                context slot as rsp, and the trampoline's syscall
 *                arrives with exactly that view — [rsp] names the
 *                context, which is the only thing sigreturn needs.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
bool frame_handler_entry(SyscallFrame& frame, int number, const SigAction& act,
                         const SigSet& blocked_before) {
    if (!stack_holds_delivery(frame.user_rsp)) {
        return false;
    }
    const unsigned long long kContext     = (frame.user_rsp - sizeof(SigReturnContext)) & ~0xFULL;
    const unsigned long long kReturnSlot  = kContext - 8;
    const unsigned long long kContextSlot = kContext - 16;
    auto& image       = *cinux::base::PtrAt<SigReturnContext>(static_cast<unsigned long>(kContext));
    image.user_rsp    = frame.user_rsp;
    image.rcx         = frame.rcx;
    image.r11         = frame.r11;
    image.rax         = frame.rax;
    image.rdi         = frame.rdi;
    image.rsi         = frame.rsi;
    image.rdx         = frame.rdx;
    image.r10         = frame.r10;
    image.r8          = frame.r8;
    image.r9          = frame.r9;
    image.rbx         = frame.rbx;
    image.rbp         = frame.rbp;
    image.r12         = frame.r12;
    image.r13         = frame.r13;
    image.r14         = frame.r14;
    image.r15         = frame.r15;
    image.old_blocked = blocked_before;
    image.reserved    = 0;
    *cinux::base::PtrAt<unsigned long long>(static_cast<unsigned long>(kReturnSlot)) =
        SignalTrampolineAddress();
    *cinux::base::PtrAt<unsigned long long>(static_cast<unsigned long>(kContextSlot)) = kContext;
    frame.user_rsp                                                                    = kReturnSlot;
    frame.rcx                                                                         = act.handler;
    frame.rdi = static_cast<unsigned long long>(number);
    frame.rax = 0;
    return true;
}

}  // namespace

bool SignalCatchable(int number) {
    return number >= 1 && number <= kSignalCount && number != cinux::syscall::kSigKill &&
           number != cinux::syscall::kSigStop;
}

bool SignalBlockable(int number) {
    return number >= 1 && number <= kSignalCount && number != cinux::syscall::kSigKill &&
           number != cinux::syscall::kSigStop;
}

SignalDisposition DefaultDisposition(int number) {
    switch (number) {
    case cinux::syscall::kSigChld:
    case cinux::syscall::kSigUsr1:
    case cinux::syscall::kSigUsr2:
        return SignalDisposition::kIgnore;
    case cinux::syscall::kSigCont:
        return SignalDisposition::kContinue;
    case cinux::syscall::kSigStop:
    case cinux::syscall::kSigTstp:
    case cinux::syscall::kSigTtin:
    case cinux::syscall::kSigTtou:
        return SignalDisposition::kStop;
    default:
        return SignalDisposition::kTerminate;
    }
}

bool SendSignal(Task& target, int number) {
    if (number < 1 || number > kSignalCount) {
        return false;
    }
    if (target.state == TaskState::kZombie || target.state == TaskState::kDead) {
        return true;
    }
    const SigAction kAction = target.signals.actions[number - 1];
    if (kAction.handler == kSignalIgnore) {
        return true;
    }
    if (number == cinux::syscall::kSigCont) {
        target.signals.pending = target.signals.pending & ~stop_class_mask();
    }
    target.signals.pending = target.signals.pending | bit_for(number);
    if (target.state == TaskState::kStopped &&
        (number == cinux::syscall::kSigCont || number == cinux::syscall::kSigKill)) {
        Scheduler::self().resume(target);
        return true;
    }
    const bool kStopsNow =
        kAction.handler == kSignalDefault && DefaultDisposition(number) == SignalDisposition::kStop;
    if (target.state == TaskState::kReady && kStopsNow) {
        Scheduler::self().withdraw(target);
    }
    return true;
}

bool DeliverPendingSignals(SyscallFrame& frame) {
    Task* const kSelf = Scheduler::self().current();
    if (kSelf == nullptr) {
        return false;
    }
    bool rewrote = false;
    for (;;) {
        const int kNumber = lowest_runnable(kSelf->signals);
        if (kNumber == 0) {
            break;
        }
        SigAction& action = kSelf->signals.actions[kNumber - 1];
        if (action.handler == kSignalIgnore) {
            kSelf->signals.pending = kSelf->signals.pending & ~bit_for(kNumber);
            continue;
        }
        if (action.handler == kSignalDefault) {
            if (DefaultDisposition(kNumber) == SignalDisposition::kTerminate) {
                die_by_signal(kNumber);
                break;
            }
            kSelf->signals.pending = kSelf->signals.pending & ~bit_for(kNumber);
            if (DefaultDisposition(kNumber) == SignalDisposition::kStop) {
                Scheduler::self().stop_current();
                break;
            }
            continue;
        }
        if (frame_handler_entry(frame, kNumber, action, kSelf->signals.blocked)) {
            kSelf->signals.blocked = kSelf->signals.blocked | action.mask;
            if ((action.flags & cinux::syscall::kSaResethand) != 0) {
                action = SigAction{};
            }
            kSelf->signals.pending = kSelf->signals.pending & ~bit_for(kNumber);
            rewrote                = true;
        } else {
            die_by_signal(kNumber);
        }
        break;
    }
    return rewrote;
}

unsigned long SignalTrampolineAddress() {
    return mm::kUserMmapTop + cinux::arch::page::kSize;
}

}  // namespace cinux::proc
