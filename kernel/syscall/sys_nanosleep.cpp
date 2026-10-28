#include "kernel/syscall/sys_nanosleep.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "kernel/syscall/wait_time.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kNsPerSecond   = 1'000'000'000ULL;
constexpr unsigned long      kTimespecBytes = sizeof(Timespec);

long long wait_until(unsigned long long deadline, unsigned long long remainder, bool absolute) {
    for (;;) {
        const auto kNow = MonotonicNanoseconds();
        if (kNow >= deadline) {
            return 0;
        }
        if (proc::HasPendingSignal(*CurrentTask())) {
            if (remainder != 0 && !absolute) {
                const auto     kLeft = deadline - kNow;
                const Timespec kRemain{
                    .seconds     = static_cast<long long>(kLeft / kNsPerSecond),
                    .nanoseconds = static_cast<long long>(kLeft % kNsPerSecond),
                };
                if (!uaccess::CopyToUser(remainder, &kRemain, sizeof(kRemain))) {
                    return -kEfault;
                }
            }
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace

long long HandleNanosleep(const SyscallFrame& frame) {
    if (frame.rdi == 0) {
        return -kEfault;
    }
    if (frame.rsi != 0 &&
        (frame.rsi >= kUserAddressLimit || !UserRangeWritable(frame.rsi, kTimespecBytes))) {
        return -kEfault;
    }
    unsigned long long deadline = 0;
    if (const auto kError = ReadWaitDeadline(frame.rdi, deadline); kError != 0) {
        return kError;
    }

    return wait_until(deadline, frame.rsi, false);
}

long long HandleClockNanosleep(const SyscallFrame& frame) {
    if (frame.rdi > 1 || frame.rsi > 1) {
        return -kEinval;
    }
    if (frame.rsi == 0) {
        auto relative = frame;
        relative.rdi  = frame.rdx;
        relative.rsi  = frame.r10;
        return HandleNanosleep(relative);
    }
    Timespec asked{};
    if (const auto kError = ReadTimespec(frame.rdx, asked); kError != 0) {
        return kError;
    }
    const auto kSeconds = static_cast<unsigned long long>(asked.seconds);
    const auto kTail    = static_cast<unsigned long long>(asked.nanoseconds);
    if (kSeconds > (~0ULL - kTail) / kNsPerSecond) {
        return -kEinval;
    }
    const auto kTarget = (kSeconds * kNsPerSecond) + kTail;
    const auto kNow    = frame.rdi == 0 ? RealtimeNanoseconds() : MonotonicNanoseconds();
    if (kTarget <= kNow) {
        return 0;
    }
    const auto kMono = MonotonicNanoseconds();
    const auto kLeft = kTarget - kNow;
    if (kLeft > ~0ULL - kMono) {
        return -kEinval;
    }
    return wait_until(kMono + kLeft, 0, true);
}

}  // namespace cinux::syscall
