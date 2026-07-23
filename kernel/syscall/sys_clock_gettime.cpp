/**
 * @file kernel/syscall/sys_clock_gettime.cpp
 * @brief sys_clock_gettime handler (F10-M1 batch 4 / P0e SMAP-layered)
 *
 * Layered (Linux-aligned):
 *   - do_clock_gettime_kernel: fills a KERNEL ktimespec from the PIT uptime.
 *     Pure kernel-to-kernel; tests call this.
 *   - sys_clock_gettime: the user boundary. do_* fills the kernel timespec,
 *     then copy_to_user stages it out. The old direct `tp->tv_sec = ...` write
 *     into the user buffer is gone.
 *
 * CinuxOS has no wall clock; both CLOCK_REALTIME and CLOCK_MONOTONIC report
 * boot-relative time (monotonic exact, realtime stand-in until an RTC source).
 */

#include "kernel/syscall/sys_clock_gettime.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0e (SMAP): copy_to_user
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/errno.hpp"

namespace cinux::syscall {

namespace {

constexpr uint64_t kClockRealtime  = 0;
constexpr uint64_t kClockMonotonic = 1;

}  // anonymous namespace

int64_t do_clock_gettime_kernel(uint64_t clk_id, ktimespec* out) {
    if (clk_id != kClockRealtime && clk_id != kClockMonotonic) {
        return -cinux::kEinval;
    }
    uint64_t ms  = cinux::drivers::PIT::get_uptime_ms();
    out->tv_sec  = static_cast<int64_t>(ms / 1000);
    out->tv_nsec = static_cast<int64_t>((ms % 1000) * 1'000'000);
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
