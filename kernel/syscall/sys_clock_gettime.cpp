/**
 * @file kernel/syscall/sys_clock_gettime.cpp
 * @brief sys_clock_gettime handler (F10-M1 batch 4 / P0e SMAP-layered; F5-M4 HPET/RTC)
 *
 * Layered (Linux-aligned):
 *   - do_clock_gettime_kernel: fills a KERNEL ktimespec.  CLOCK_MONOTONIC reads
 *     the HPET free-running counter (boot-relative ns); CLOCK_REALTIME anchors
 *     the RTC's coarse boot second and refines it with the HPET monotonic delta
 *     (the "drift correction" -- sub-second precision plus continuous advance,
 *     since the RTC itself is not re-read per call).
 *   - sys_clock_gettime: the user boundary. do_* fills the kernel timespec,
 *     then copy_to_user stages it out.
 *
 * The HPET is primary; if it was not brought up, MONOTONIC degrades to the PIT
 * uptime the kernel already keeps (no behavioural regression on HPET-less HW).
 * If the RTC was not read at boot, REALTIME falls back to the monotonic value
 * alone.  Neither failure crashes.
 */

#include "kernel/syscall/sys_clock_gettime.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0e (SMAP): copy_to_user
#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/drivers/pit/pit.hpp"  // monotonic fallback when HPET is absent
#include "kernel/drivers/rtc/rtc.hpp"
#include "kernel/errno.hpp"

namespace cinux::syscall {

namespace {

constexpr uint64_t kClockRealtime  = 0;
constexpr uint64_t kClockMonotonic = 1;
constexpr uint64_t kNsPerSec       = 1'000'000'000ULL;
constexpr uint64_t kNsPerMs        = 1'000'000ULL;

/// Boot-relative monotonic nanoseconds, HPET-backed with a PIT fallback.
uint64_t monotonic_ns() {
    if (cinux::drivers::g_hpet.available()) {
        return cinux::drivers::g_hpet.monotonic_ns();
    }
    return cinux::drivers::PIT::get_uptime_ms() * kNsPerMs;
}

}  // anonymous namespace

int64_t do_clock_gettime_kernel(uint64_t clk_id, ktimespec* out) {
    if (clk_id != kClockRealtime && clk_id != kClockMonotonic) {
        return -cinux::kEinval;
    }
    // CLOCK_REALTIME: RTC boot epoch (coarse second) + HPET monotonic delta.
    // CLOCK_MONOTONIC: the HPET monotonic delta alone (boot-relative).
    const uint64_t mono = monotonic_ns();
    const uint64_t ns =
        (clk_id == kClockRealtime) ? (cinux::drivers::g_rtc.boot_epoch_ns() + mono) : mono;
    out->tv_sec  = static_cast<int64_t>(ns / kNsPerSec);
    out->tv_nsec = static_cast<int64_t>(ns % kNsPerSec);
    return 0;
}

int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_virt, uint64_t, uint64_t, uint64_t,
                          uint64_t) {
    ktimespec kts;
    int64_t   rc = do_clock_gettime_kernel(clk_id, &kts);
    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(tp_virt), &kts, sizeof(kts))) {
        return -cinux::kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
