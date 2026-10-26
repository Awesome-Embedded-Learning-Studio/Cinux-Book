/**
 * @file    sys_tgkill.hpp
 * @brief   tgkill(2): one signal, one thread, by both ids.
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
 * @brief         Sends one signal to one thread.
 *
 * @param[in]     frame   rdi the thread group id, rsi the thread id,
 *                        rdx the signal number.
 * @return        Zero, or a negative errno.
 * @note          The thread must belong to the supplied thread group.
 *                Signal zero only checks that the target exists.
 * @since         0.5.0
 * @ingroup       kernel_syscall
 */
long long HandleTgkill(const SyscallFrame& frame);

/**
 * @brief Sends a signal to a thread without a thread-group constraint.
 * @param[in] frame Thread ID in rdi and signal in rsi.
 * @return Zero or a negative errno.
 */
long long HandleTkill(const SyscallFrame& frame);

}  // namespace cinux::syscall
