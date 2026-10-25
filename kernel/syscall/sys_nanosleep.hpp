/**
 * @file    sys_nanosleep.hpp
 * @brief   nanosleep(2): park the caller until the clock says so.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Sleeps for the asked span, or until a signal lands.
 *
 * @param[in]     frame   rdi the timespec asked, rsi the remainder out,
 *                        may be null.
 * @return        Zero on timeout, or a negative errno.
 * @note          The wait runs on cooperative yields against the
 *                monotonic clock; an unblocked pending signal wakes
 *                the caller with -EINTR and, when a remainder address
 *                was given, the unslept span filled in. A span past
 *                the clock's range sleeps forever's honest cousin:
 *                nothing.
 * @since         0.5.0
 * @ingroup       kernel_syscall
 */
long long HandleNanosleep(const SyscallFrame& frame);

/**
 * @brief Sleeps on a selected clock with relative or absolute time.
 * @param[in] frame Linux clock_nanosleep register arguments.
 * @return Zero or a negative errno.
 * @since 0.8.0
 * @ingroup kernel_syscall
 */
long long HandleClockNanosleep(const SyscallFrame& frame);

}  // namespace cinux::syscall
