/**
 * @file    sys_itimer.hpp
 * @brief   Declarations of the real-interval-timer syscall handlers.
 *
 * The three doors a process times itself through: set the timer and
 * read the old one, read it without touching it, and the seconds-only
 * spelling alarm() kept. All three speak about the same per-process
 * ITIMER_REAL, whose expiry raises SIGALRM through the scheduler's
 * deadline scan. The other two interval timers this kernel does not
 * keep and says so with EINVAL.
 *
 * @author  Charliechen114514
 * @date    2026-10-24
 * @version 0.1
 * @since   0.7.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         getitimer(which, out): read the real timer without touching it.
 *
 * @return        Zero with the record filled, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleGetitimer(const SyscallFrame& frame);

/**
 * @brief         setitimer(which, asked, old): replace the real timer,
 *                optionally reporting the previous one.
 *
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleSetitimer(const SyscallFrame& frame);

/**
 * @brief         alarm(seconds): the seconds-only spelling of a
 *                single-shot real timer.
 *
 * @return        The rounded-up seconds that remained, zero when none
 *                was armed, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleAlarm(const SyscallFrame& frame);

}  // namespace cinux::syscall
