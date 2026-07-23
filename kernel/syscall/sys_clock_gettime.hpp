/**
 * @file kernel/syscall/sys_clock_gettime.hpp
 * @brief sys_clock_gettime handler declaration (F10-M1 batch 4)
 *
 * Reads a clock into a user timespec.  musl's time/clock support paths call
 * it during startup and from timing helpers.  Both realtime and monotonic
 * clocks are backed by the PIT uptime counter.
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// Linux struct timespec layout on x86-64: { time_t tv_sec; long tv_nsec; }.
struct ktimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/// clock_gettime(clk_id, tp) -- fill tp from the uptime clock; returns 0.
int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_virt, uint64_t, uint64_t, uint64_t,
                          uint64_t);

/// P0e (SMAP): fill a KERNEL ktimespec from the uptime clock (no user memory).
/// Tests call this; sys_clock_gettime is the user boundary (copy_to_user).
int64_t do_clock_gettime_kernel(uint64_t clk_id, ktimespec* out);

}  // namespace cinux::syscall
