/**
 * @file    sys_futex.hpp
 * @brief   Declaration of the futex() syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.1
 * @since   0.8.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Park the caller on, or wake waiters of, a user word.
 *
 * @return        For a wait: zero once woken, -kEagain when the word
 *                moved on first. For a wake: how many were woken. Or
 *                a negative errno (-kEinval for an unknown op or a
 *                misaligned word, -kEfault for a kernel-half word).
 * @since         0.8.0
 * @ingroup       kernel_syscall
 */
long long HandleFutex(const SyscallFrame& frame);

}  // namespace cinux::syscall
