#include "kernel/proc/signals.hpp"

#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"

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

/// Preserve the interrupted function's red zone as well as alignment
/// slack, the saved context and the handler's return address.
constexpr unsigned long long kRedZoneBytes  = 128;
constexpr unsigned long long kDeliveryBytes = kRedZoneBytes + 15 + sizeof(SigReturnContext) + 8;

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
    const Task* const kSelf = Scheduler::self().current();
    return rsp >= kDeliveryBytes && kSelf != nullptr && kSelf->vmas.holds() &&
           kSelf->vmas->stack_contains(rsp - kDeliveryBytes, kDeliveryBytes);
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
 * @note          The layout under the interrupted red zone, low to high:
 *                the return address, then the context itself. The frame
 *                then reads like a call just happened: the stack
 *                pointer rests on the return slot (eight mod sixteen,
 *                as the ABI demands after a call), rcx carries the
 *                handler, rdi the signal number. The handler's ret
 *                lands on the return word — the action's own restorer
 *                when SA_RESTORER promised one, the kernel trampoline
 *                page otherwise — with the context itself as rsp.
 *                Nothing saved below the return address may survive an
 *                ordinary handler's pushes.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
bool frame_handler_entry(SyscallFrame& frame, int number, const SigAction& act,
                         const SigSet& blocked_before) {
    if (!stack_holds_delivery(frame.user_rsp)) {
        return false;
    }
    const unsigned long long kContext =
        (frame.user_rsp - kRedZoneBytes - sizeof(SigReturnContext)) & ~0xFULL;
    const unsigned long long kReturnSlot = kContext - 8;
    SigReturnContext         image{};
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
    const unsigned long long kReturnAddress =
        (act.flags & cinux::syscall::kSaRestorer) != 0 ? act.restorer : SignalTrampolineAddress();
    if (!cinux::syscall::uaccess::CopyToUser(kContext, &image, sizeof(image)) ||
        !cinux::syscall::uaccess::CopyToUser(kReturnSlot, &kReturnAddress,
                                             sizeof(kReturnAddress))) {
        return false;
    }
    frame.user_rsp = kReturnSlot;
    frame.rcx      = act.handler;
    frame.rdi      = static_cast<unsigned long long>(number);
    frame.rax      = 0;
    return true;
}

/// Clears one pending bit under the gate; senders on other cores hold
/// the same gate for their rewrites, so a clear never eats a send.
void clear_pending_bit(SignalState& state, int number) {
    const auto kGate = state.gate.guard();
    state.pending    = state.pending & ~bit_for(number);
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

void SignalState::reset() {
    const auto kGate = gate.guard();
    for (SigAction& act : actions) {
        act = SigAction{};
    }
    blocked = {};
    pending = {};
}

void SendToGroup(unsigned long long pgid, int number) {
    Scheduler::self().for_each_task([&](Task& scan) {
        if (scan.pgid == pgid) {
            static_cast<void>(SendSignal(scan, number));
        }
    });
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
    {
        const auto kGate = target.signals.gate.guard();
        if (number == cinux::syscall::kSigCont) {
            target.signals.pending = target.signals.pending & ~stop_class_mask();
        }
        target.signals.pending = target.signals.pending | bit_for(number);
    }
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
            clear_pending_bit(kSelf->signals, kNumber);
            continue;
        }
        if (action.handler == kSignalDefault) {
            if (DefaultDisposition(kNumber) == SignalDisposition::kTerminate) {
                die_by_signal(kNumber);
                break;
            }
            clear_pending_bit(kSelf->signals, kNumber);
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
            clear_pending_bit(kSelf->signals, kNumber);
            rewrote = true;
        } else {
            die_by_signal(cinux::syscall::kSigSegv);
        }
        break;
    }
    return rewrote;
}

unsigned long SignalTrampolineAddress() {
    return mm::kUserMmapTop + cinux::arch::page::kSize;
}

}  // namespace cinux::proc
