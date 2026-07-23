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

/// clock_gettime(clk_id, tp) -- fill tp from the uptime clock; returns 0.
int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_virt, uint64_t, uint64_t, uint64_t,
                          uint64_t);

}  // namespace cinux::syscall
