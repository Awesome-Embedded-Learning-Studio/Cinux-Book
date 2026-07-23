/**
 * @file kernel/syscall/sys_clock_gettime.cpp
 * @brief sys_clock_gettime handler implementation (F10-M1 batch 4)
 *
 * Fills a user timespec from the PIT uptime counter.  CinuxOS has no wall
 * clock yet, so both CLOCK_REALTIME and CLOCK_MONOTONIC report boot-relative
 * time -- monotonic is exact, realtime is a stand-in until an RTC source is
 * wired.  musl does not crash on a coarse clock.
 */

#include "kernel/syscall/sys_clock_gettime.hpp"

#include <stdint.h>

#include "kernel/drivers/pit/pit.hpp"
#include "kernel/errno.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

constexpr uint64_t kClockRealtime  = 0;
constexpr uint64_t kClockMonotonic = 1;

/// Linux struct timespec layout on x86-64: { time_t tv_sec; long tv_nsec; }.
struct ktimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

}  // anonymous namespace

int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_virt, uint64_t, uint64_t, uint64_t,
                          uint64_t) {
    if (clk_id != kClockRealtime && clk_id != kClockMonotonic) {
        return -cinux::kEinval;
    }
    if (!validate_user_ptr(tp_virt)) {
        return -cinux::kEfault;
    }

    uint64_t ms = cinux::drivers::PIT::get_uptime_ms();
    auto*    tp = reinterpret_cast<ktimespec*>(tp_virt);
    tp->tv_sec  = static_cast<int64_t>(ms / 1000);
    tp->tv_nsec = static_cast<int64_t>((ms % 1000) * 1'000'000);

    return 0;
}

}  // namespace cinux::syscall
