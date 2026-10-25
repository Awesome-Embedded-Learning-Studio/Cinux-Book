#include "kernel/syscall/sys_nanosleep.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kNsPerSecond   = 1'000'000'000ULL;
constexpr unsigned long      kTimespecBytes = 16;

long long wait_until(unsigned long long deadline, unsigned long long remainder, bool absolute) {
    for (;;) {
        const auto kNow = MonotonicNanoseconds();
        if (kNow >= deadline) {
            return 0;
        }
        if (proc::HasPendingSignal(*CurrentTask())) {
            if (remainder != 0 && !absolute) {
                const auto               kLeft      = deadline - kNow;
                const unsigned long long kRemain[2] = {kLeft / kNsPerSecond, kLeft % kNsPerSecond};
                if (!uaccess::CopyToUser(remainder, kRemain, sizeof(kRemain))) {
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
    unsigned long long asked[2] = {};
    if (!uaccess::CopyFromUser(asked, frame.rdi, kTimespecBytes)) {
        return -kEfault;
    }
    if (asked[0] >= (~0ULL / kNsPerSecond) || asked[1] >= kNsPerSecond) {
        return -kEinval;
    }
    const unsigned long long kNow   = MonotonicNanoseconds();
    const unsigned long long kSlack = ~0ULL - kNow;
    if (asked[1] > kSlack || asked[0] > ((kSlack - asked[1]) / kNsPerSecond)) {
        return 0;
    }
    const unsigned long long kDeadline = kNow + (asked[0] * kNsPerSecond) + asked[1];

    return wait_until(kDeadline, frame.rsi, false);
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
    unsigned long long asked[2]{};
    if (!uaccess::CopyFromUser(asked, frame.rdx, sizeof(asked))) {
        return -kEfault;
    }
    if (asked[0] > ~0ULL / kNsPerSecond || asked[1] >= kNsPerSecond ||
        asked[1] > ~0ULL - (asked[0] * kNsPerSecond)) {
        return -kEinval;
    }
    const auto kTarget = (asked[0] * kNsPerSecond) + asked[1];
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
