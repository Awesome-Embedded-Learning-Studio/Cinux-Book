/**
 * @file    sys_arch_prctl.hpp
 * @brief   Declaration of the arch_prctl() syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Move one of the caller's thread-pointer bases.
 *
 * @return        Zero on success, or a negative errno (-kEinval for
 *                an unknown op or a non-canonical base, -kEfault when
 *                a get lands in the kernel half).
 * @since         0.8.0
 * @ingroup       kernel_syscall
 */
long long HandleArchPrctl(const SyscallFrame& frame);

}  // namespace cinux::syscall
