/**
 * @file    sys_fcntl.hpp
 * @brief   Declaration of the fcntl(fd, cmd, arg) syscall handler.
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
 * @brief         Descriptor knobs: close-on-exec flags and status bits.
 *
 * @return        The knob's answer for reads, zero for writes, or a
 *                negative errno.
 * @note          The hosted runtime sets FD_CLOEXEC on every open so an
 *                exec leaves no trail; descriptors here do not survive
 *                exec at all, so the offers are accepted and kept as
 *                no-ops, and reads report a plain read-write status.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleFcntl(const SyscallFrame& frame);

}  // namespace cinux::syscall
