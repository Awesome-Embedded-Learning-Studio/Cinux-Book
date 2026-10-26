/**
 * @file    sys_wait.hpp
 * @brief   Declaration of the wait4(pid, status) syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Block until one child exits, then retire it.
 *
 * @return        The reaped child's pid, or a negative errno.
 * @note          The status word carries the Linux wait encoding: an
 *                exit value in the high byte, or a terminating signal
 *                in the low byte.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleWait4(const SyscallFrame& frame);

/**
 * @brief Reaps a child selected by Linux waitid's identity convention.
 * @param[in] frame rdi selects the identity kind, rsi the identity,
 *                  rdx the optional siginfo record, r10 the options.
 * @return Zero on success, or a negative errno.
 * @since 0.8.0
 * @ingroup kernel_syscall
 */
long long HandleWaitid(const SyscallFrame& frame);

}  // namespace cinux::syscall
