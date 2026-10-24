#include "kernel/syscall/sys_nanosleep.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kNsPerSecond   = 1'000'000'000ULL;
constexpr unsigned long      kTimespecBytes = 16;

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

    for (;;) {
        const proc::Task* const kSelf = CurrentTask();
        if (proc::HasPendingSignal(*kSelf)) {
            if (frame.rsi != 0) {
                const unsigned long long kLeft =
                    kDeadline > MonotonicNanoseconds() ? kDeadline - MonotonicNanoseconds() : 0;
                const unsigned long long kRemain[2] = {kLeft / kNsPerSecond, kLeft % kNsPerSecond};
                if (!uaccess::CopyToUser(frame.rsi, kRemain, sizeof(kRemain))) {
                    return -kEfault;
                }
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
