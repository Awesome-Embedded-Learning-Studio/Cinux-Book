/**
 * @file    sys_clock.hpp
 * @brief   Declarations of the wall-and-monotonic clock syscall handlers.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         time(tloc): seconds since the epoch.
 *
 * @return        The seconds, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleTime(const SyscallFrame& frame);

/**
 * @brief         gettimeofday(tv, tz): wall seconds and microseconds.
 *
 * @return        Zero on success, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleGettimeofday(const SyscallFrame& frame);

/**
 * @brief         clock_gettime(clk, tp): seconds and nanoseconds.
 *
 * @return        Zero on success, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleClockGettime(const SyscallFrame& frame);

/**
 * @brief         Monotonic nanoseconds since boot, finest source first.
 *
 * @return        Elapsed nanoseconds on the always-running clock.
 * @note          The HPET's fine hand when the firmware named one, the
 *                heartbeat's coarse ten-millisecond steps otherwise.
 *                One source for every caller that measures elapsed
 *                time, timeouts included.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
unsigned long long MonotonicNanoseconds();

}  // namespace cinux::syscall
