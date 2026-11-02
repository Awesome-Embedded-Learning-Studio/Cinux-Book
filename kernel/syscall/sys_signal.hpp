/**
 * @file    sys_signal.hpp
 * @brief   Declarations of the signal syscall handlers.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Return pending blocked signals in the caller's kernel-sized set.
 * @return The result or a negative Linux errno.
 */
long long HandleRtSigpending(const SyscallFrame& frame);

/**
 * @brief         Install (or ask about) one signal's disposition.
 * @param[in]     frame   rdi the signal, rsi the action to install
 *                        (null asks only), rdx the old action out
 *                        (null skips), r10 the sigset size in bytes.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleRtSigaction(const SyscallFrame& frame);

/**
 * @brief         Move the caller's blocked set.
 * @param[in]     frame   rdi the move kind, rsi the set to apply
 *                        (null keeps the mask), rdx the old mask out
 *                        (null skips), r10 the sigset size in bytes.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleRtSigprocmask(const SyscallFrame& frame);

/**
 * @brief         The trampoline's way home: restore what delivery saved.
 * @param[in,out] frame   user_rsp names the slot holding the context
 *                        address; the sixteen saved words flow back
 *                        into the frame, the saved mask back onto the
 *                        task.
 * @return        The interrupted syscall's answer, which the caller
 *                asked to see again in rax.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleRtSigreturn(const SyscallFrame& frame);

}  // namespace cinux::syscall
