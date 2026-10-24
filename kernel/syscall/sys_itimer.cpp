#include "kernel/syscall/sys_itimer.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {
namespace {
constexpr Word kNanosecondsPerSecond      = 1'000'000'000;
constexpr Word kNanosecondsPerMicrosecond = 1'000;
struct Timeval {
    long long seconds      = 0;
    long long microseconds = 0;
};
struct Itimerval {
    Timeval interval;
    Timeval value;
};
static_assert(sizeof(Itimerval) == 32);

bool to_nanoseconds(const Timeval& value, Word& out) {
    if (value.seconds < 0 || value.microseconds < 0 || value.microseconds >= 1'000'000) {
        return false;
    }
    const auto kTail = static_cast<Word>(value.microseconds) * kNanosecondsPerMicrosecond;
    if (static_cast<Word>(value.seconds) > (~0ULL - kTail) / kNanosecondsPerSecond) {
        return false;
    }
    out = (static_cast<Word>(value.seconds) * kNanosecondsPerSecond) + kTail;
    return true;
}

Timeval from_nanoseconds(Word value) {
    const Word kMicros =
        (value / kNanosecondsPerMicrosecond) + (value % kNanosecondsPerMicrosecond != 0 ? 1 : 0);
    return {
        .seconds      = static_cast<long long>(kMicros / 1'000'000),
        .microseconds = static_cast<long long>(kMicros % 1'000'000),
    };
}
}

long long HandleGetitimer(const SyscallFrame& frame) {
    if (frame.rdi != 0) {
        return -kEinval;
    }
    proc::RealTimerValue timer;
    proc::Scheduler::self().real_timer(*CurrentTask(), MonotonicNanoseconds(), timer, false);
    const Itimerval kOld{
        .interval = from_nanoseconds(timer.interval_ns),
        .value    = from_nanoseconds(timer.value_ns),
    };
    return uaccess::CopyToUser(frame.rsi, &kOld, sizeof(kOld)) ? 0 : -kEfault;
}

long long HandleSetitimer(const SyscallFrame& frame) {
    if (frame.rdi != 0) {
        return -kEinval;
    }
    Itimerval wanted{};
    if (frame.rsi != 0 && !uaccess::CopyFromUser(&wanted, frame.rsi, sizeof(wanted))) {
        return -kEfault;
    }
    proc::RealTimerValue timer;
    const Word           kNow = MonotonicNanoseconds();
    if (!to_nanoseconds(wanted.value, timer.value_ns) ||
        !to_nanoseconds(wanted.interval, timer.interval_ns) || timer.value_ns > ~0ULL - kNow) {
        return -kEinval;
    }
    proc::Scheduler::self().real_timer(*CurrentTask(), kNow, timer, true);
    if (frame.rdx != 0) {
        const Itimerval kOld{
            .interval = from_nanoseconds(timer.interval_ns),
            .value    = from_nanoseconds(timer.value_ns),
        };
        if (!uaccess::CopyToUser(frame.rdx, &kOld, sizeof(kOld))) {
            return -kEfault;
        }
    }
    return 0;
}

long long HandleAlarm(const SyscallFrame& frame) {
    proc::RealTimerValue timer{
        .value_ns = static_cast<unsigned int>(frame.rdi) * kNanosecondsPerSecond,
    };
    proc::Scheduler::self().real_timer(*CurrentTask(), MonotonicNanoseconds(), timer, true);
    return static_cast<long long>((timer.value_ns / kNanosecondsPerSecond) +
                                  (timer.value_ns % kNanosecondsPerSecond != 0 ? 1 : 0));
}

}
