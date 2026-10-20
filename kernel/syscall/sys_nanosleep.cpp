#include "kernel/syscall/sys_nanosleep.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kNsPerSecond   = 1'000'000'000ULL;
constexpr unsigned long      kTimespecBytes = 16;

}  // namespace

long long HandleNanosleep(const SyscallFrame& frame) {
    if (frame.rdi == 0 || frame.rdi >= kUserAddressLimit) {
        return -kEfault;
    }
    if (!UserRangeReadable(frame.rdi, kTimespecBytes)) {
        return -kEfault;
    }
    if (frame.rsi != 0 &&
        (frame.rsi >= kUserAddressLimit || !UserRangeWritable(frame.rsi, kTimespecBytes))) {
        return -kEfault;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto* const kAsked = reinterpret_cast<const unsigned long long*>(frame.rdi);
    if (kAsked[0] >= (~0ULL / kNsPerSecond) || kAsked[1] >= kNsPerSecond) {
        return -kEinval;
    }
    const unsigned long long kNow   = MonotonicNanoseconds();
    const unsigned long long kSlack = ~0ULL - kNow;
    if (kAsked[1] > kSlack || kAsked[0] > ((kSlack - kAsked[1]) / kNsPerSecond)) {
        return 0;
    }
    const unsigned long long kDeadline = kNow + (kAsked[0] * kNsPerSecond) + kAsked[1];

    for (;;) {
        const proc::Task* const kSelf = CurrentTask();
        if (kSelf->signals.pending.has(~kSelf->signals.blocked)) {
            if (frame.rsi != 0) {
                const unsigned long long kLeft =
                    kDeadline > MonotonicNanoseconds() ? kDeadline - MonotonicNanoseconds() : 0;
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                auto* const kRemain = reinterpret_cast<unsigned long long*>(frame.rsi);
                kRemain[0]          = kLeft / kNsPerSecond;
                kRemain[1]          = kLeft % kNsPerSecond;
            }
            return -kEintr;
        }
        if (MonotonicNanoseconds() >= kDeadline) {
            return 0;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace cinux::syscall
