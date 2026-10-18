/**
 * @file    sys_pipe.hpp
 * @brief   Declarations of the pipe and descriptor-copy syscall handlers.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         pipe(fds): one ring, two descriptors, read end first.
 *
 * @return        Zero when both ends land, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandlePipe(const SyscallFrame& frame);

/**
 * @brief         pipe2(fds, flags): the same ring; served flags none,
 *                zero only.
 *
 * @return        Zero with a zero flag word, or -EINVAL for anything
 *                else.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandlePipe2(const SyscallFrame& frame);

/**
 * @brief         dup(fd): the same open under the lowest free number.
 *
 * @return        The new descriptor number, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleDup(const SyscallFrame& frame);

/**
 * @brief         dup2(fd, target): the same open under target,
 *                closing what stood there.
 *
 * @return        The target number, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleDup2(const SyscallFrame& frame);

}  // namespace cinux::syscall
