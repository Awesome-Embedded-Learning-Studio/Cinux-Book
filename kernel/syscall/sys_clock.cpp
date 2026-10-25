#include "kernel/syscall/sys_clock.hpp"

#include "kernel/driver/hpet/hpet.hpp"
#include "kernel/driver/rtc/rtc.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "kernel/time/tick.hpp"

namespace cinux::syscall {

namespace {

/// Wall seconds at boot, asked of the RTC once and never again: the
/// hardware read is slow and the wall only moves forward from here.
unsigned long long boot_wall() {
    static const unsigned long long kRead = driver::Rtc::self().now_seconds();
    return kRead;
}

/// Splits monotonic nanoseconds into whole seconds and the tail.
struct ClockSplit {
    unsigned long long seconds;
    unsigned long long nanoseconds_past_second;
};

ClockSplit split_ns(unsigned long long total_ns) {
    return ClockSplit{
        .seconds                 = total_ns / 1'000'000'000ULL,
        .nanoseconds_past_second = total_ns % 1'000'000'000ULL,
    };
}

}  // namespace

unsigned long long MonotonicNanoseconds() {
    const driver::Hpet& fine = driver::Hpet::self();
    if (fine.present()) {
        return fine.since_boot_ns();
    }
    const unsigned long long kTicks = time::Tick::self().since_boot();
    return kTicks * 10'000'000ULL;
}

unsigned long long RealtimeNanoseconds() {
    return (boot_wall() * 1'000'000'000ULL) + MonotonicNanoseconds();
}

long long HandleTime(const SyscallFrame& frame) {
    const unsigned long long kNow = boot_wall() + split_ns(MonotonicNanoseconds()).seconds;
    if (frame.rdi != 0 && !uaccess::CopyToUser(frame.rdi, &kNow, sizeof(kNow))) {
        return -kEfault;
    }
    return static_cast<long long>(kNow);
}

long long HandleGettimeofday(const SyscallFrame& frame) {
    if (frame.rdi == 0 || frame.rdi >= kUserAddressLimit) {
        return -kEfault;
    }
    const ClockSplit         kSince   = split_ns(MonotonicNanoseconds());
    const unsigned long long kPair[2] = {
        boot_wall() + kSince.seconds,
        kSince.nanoseconds_past_second / 1'000ULL,
    };
    if (!uaccess::CopyToUser(frame.rdi, kPair, sizeof(kPair))) {
        return -kEfault;
    }
    return 0;
}

long long HandleClockGettime(const SyscallFrame& frame) {
    if (frame.rsi == 0) {
        return -kEfault;
    }
    constexpr unsigned long long kClockRealtime  = 0;
    constexpr unsigned long long kClockMonotonic = 1;
    const ClockSplit             kSince          = split_ns(MonotonicNanoseconds());
    unsigned long long           seconds         = kSince.seconds;
    if (frame.rdi == kClockRealtime) {
        seconds += boot_wall();
    } else if (frame.rdi != kClockMonotonic) {
        return -kEinval;
    }
    const unsigned long long kPair[2] = {seconds, kSince.nanoseconds_past_second};
    if (!uaccess::CopyToUser(frame.rsi, kPair, sizeof(kPair))) {
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
