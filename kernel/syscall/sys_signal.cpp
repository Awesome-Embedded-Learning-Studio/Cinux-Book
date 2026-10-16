#include "kernel/syscall/sys_signal.hpp"

#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// rt_sigprocmask's how words: add to the mask, remove from it, replace it.
constexpr Word kSigBlock   = 0;
constexpr Word kSigUnblock = 1;
constexpr Word kSigSetmask = 2;

/// One action record crosses the boundary whole.
constexpr Word kActionBytes = sizeof(SigAction);

/// One sigset word crosses the boundary whole.
constexpr Word kSetBytes = sizeof(SigSet);

/**
 * @brief         Clears the two signals no mask may hold.
 * @param[in]     set   The mask ring 3 offered.
 * @return        The same mask without the kill and stop bits.
 * @note          Linux strips these silently rather than refusing: a
 *                library that blocks "everything" should not have to
 *                know which two signals the kernel keeps for itself.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
SigSet strip_uncatchable(SigSet set) {
    set = set & ~SigSet{1ULL << static_cast<unsigned>(kSigKill - 1)};
    set = set & ~SigSet{1ULL << static_cast<unsigned>(kSigStop - 1)};
    return set;
}

/**
 * @brief         Whether a handler word names something ring 3 owns.
 * @param[in]     handler   The word an action record carries.
 * @return        true for the reserved default and ignore words and
 *                for any address inside the user half.
 * @note          The kernel never checks the address further: a
 *                handler in an unmapped page faults on delivery, and
 *                that fault is the caller's, not the installer's.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
bool handler_word_ok(Word handler) {
    return handler < kUserAddressLimit;
}

/**
 * @brief         Whether a stack address may hold delivery images.
 * @param[in]     start   First byte of the stretch.
 * @param[in]     bytes   How many bytes the stretch covers.
 * @return        true when the whole stretch sits inside the
 *                registered stack window.
 * @note          The sigreturn half of delivery's own guard: the
 *                context only ever lived under the interrupted stack
 *                pointer, so a "context" anywhere else is a corrupted
 *                stack, and a corrupted stack is how this task dies.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
bool in_stack_window(Word start, Word bytes) {
    return start >= mm::kUserStackBottom && bytes <= mm::kUserStackTop - start;
}

}  // namespace

long long HandleRtSigaction(const SyscallFrame& frame) {
    const int kNumber = static_cast<int>(frame.rdi);
    if (!proc::SignalCatchable(kNumber) || frame.r10 != kSetBytes) {
        return -kEinval;
    }
    SigAction wanted{};
    bool      install = false;
    if (frame.rsi != 0) {
        if (!UserRangeReadable(frame.rsi, kActionBytes)) {
            return -kEfault;
        }
        cinux::base::CopyBytes(&wanted, cinux::base::PtrAt<void>(frame.rsi), kActionBytes);
        if (!handler_word_ok(wanted.handler)) {
            return -kEinval;
        }
        wanted.mask = strip_uncatchable(wanted.mask);
        install     = true;
    }
    proc::Task&     self    = *CurrentTask();
    const SigAction kBefore = self.signals.actions[kNumber - 1];
    if (frame.rdx != 0) {
        if (!UserRangeWritable(frame.rdx, kActionBytes)) {
            return -kEfault;
        }
        cinux::base::CopyBytes(cinux::base::PtrAt<void>(frame.rdx), &kBefore, kActionBytes);
    }
    if (install) {
        self.signals.actions[kNumber - 1] = wanted;
    }
    return 0;
}

long long HandleRtSigprocmask(const SyscallFrame& frame) {
    if (frame.r10 != kSetBytes) {
        return -kEinval;
    }
    SigSet wanted{};
    bool   apply = false;
    if (frame.rsi != 0) {
        if (frame.rdi > kSigSetmask) {
            return -kEinval;
        }
        if (!UserRangeReadable(frame.rsi, kSetBytes)) {
            return -kEfault;
        }
        cinux::base::CopyBytes(&wanted, cinux::base::PtrAt<void>(frame.rsi), kSetBytes);
        wanted = strip_uncatchable(wanted);
        apply  = true;
    }
    proc::Task& self = *CurrentTask();
    if (frame.rdx != 0) {
        const SigSet kNow = self.signals.blocked;
        if (!UserRangeWritable(frame.rdx, kSetBytes)) {
            return -kEfault;
        }
        cinux::base::CopyBytes(cinux::base::PtrAt<void>(frame.rdx), &kNow, kSetBytes);
    }
    if (!apply) {
        return 0;
    }
    switch (frame.rdi) {
    case kSigBlock:
        self.signals.blocked = self.signals.blocked | wanted;
        break;
    case kSigUnblock:
        self.signals.blocked = self.signals.blocked & ~wanted;
        break;
    default:
        self.signals.blocked = wanted;
        break;
    }
    return 0;
}

long long HandleRtSigreturn(const SyscallFrame& frame) {
    const Word kSlot = frame.user_rsp;
    if (!in_stack_window(kSlot, kSetBytes) || (kSlot & 0xFULL) != 0) {
        proc::Scheduler::self().exit_current(kSigSegv);
        return 0;
    }
    const Word kContext = *cinux::base::PtrAt<Word>(static_cast<unsigned long>(kSlot));
    if (!in_stack_window(kContext, sizeof(proc::SigReturnContext)) || (kContext & 0xFULL) != 0) {
        proc::Scheduler::self().exit_current(kSigSegv);
        return 0;
    }
    proc::SigReturnContext image{};
    cinux::base::CopyBytes(&image, cinux::base::PtrAt<void>(kContext),
                           sizeof(proc::SigReturnContext));
    auto& mutable_frame            = const_cast<SyscallFrame&>(frame);
    mutable_frame.user_rsp         = image.user_rsp;
    mutable_frame.rcx              = image.rcx;
    mutable_frame.r11              = image.r11;
    mutable_frame.rax              = image.rax;
    mutable_frame.rdi              = image.rdi;
    mutable_frame.rsi              = image.rsi;
    mutable_frame.rdx              = image.rdx;
    mutable_frame.r10              = image.r10;
    mutable_frame.r8               = image.r8;
    mutable_frame.r9               = image.r9;
    mutable_frame.rbx              = image.rbx;
    mutable_frame.rbp              = image.rbp;
    mutable_frame.r12              = image.r12;
    mutable_frame.r13              = image.r13;
    mutable_frame.r14              = image.r14;
    mutable_frame.r15              = image.r15;
    CurrentTask()->signals.blocked = image.old_blocked;
    return static_cast<long long>(image.rax);
}

}  // namespace cinux::syscall
