#include "kernel/syscall/sys_sigtimedwait.hpp"

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "kernel/syscall/wait_time.hpp"

namespace cinux::syscall {

namespace {

struct SignalInfo {
    int                number   = 0;
    int                error    = 0;
    int                code     = 0;
    int                reserved = 0;
    unsigned long long payload[14]{};
};
static_assert(sizeof(SignalInfo) == 128);

int take_pending(proc::Task& task, SigSet wanted) {
    const arch::IrqGuard kIrq;
    const auto           kGate    = task.signals.gate.guard();
    const auto           kPending = task.signals.pending & wanted;
    for (int number = 1; number <= kSignalCount; ++number) {
        const SigSet kBit{
            base::bit::MaskBit<unsigned long long>(static_cast<unsigned>(number - 1)),
        };
        if (kPending.has(kBit)) {
            task.signals.pending = task.signals.pending & ~kBit;
            return number;
        }
    }
    return 0;
}

}

long long HandleRtSigtimedwait(const SyscallFrame& frame) {
    if (frame.r10 != sizeof(SigSet)) {
        return -kEinval;
    }
    SigSet wanted{};
    if (!uaccess::CopyFromUser(&wanted, frame.rdi, sizeof(wanted))) {
        return -kEfault;
    }
    SignalInfo info{};
    if (frame.rsi != 0 && !UserRangeWritable(frame.rsi, sizeof(info))) {
        return -kEfault;
    }
    wanted                       = MaskableSignals(wanted);
    unsigned long long deadline  = ~0ULL;
    const auto         kDeadline = ReadWaitDeadline(frame.rdx, deadline);
    if (kDeadline != 0) {
        return kDeadline;
    }
    auto& self = *CurrentTask();
    for (;;) {
        const auto kNumber = take_pending(self, wanted);
        if (kNumber != 0) {
            info.number = kNumber;
            if (frame.rsi != 0 && !uaccess::CopyToUser(frame.rsi, &info, sizeof(info))) {
                return -kEfault;
            }
            return kNumber;
        }
        if (proc::HasPendingSignal(self)) {
            return -kEintr;
        }
        if (MonotonicNanoseconds() >= deadline) {
            return -kEagain;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace cinux::syscall
