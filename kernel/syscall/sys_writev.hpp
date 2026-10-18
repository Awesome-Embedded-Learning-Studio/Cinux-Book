/**
 * @file    sys_writev.hpp
 * @brief   Declaration of the writev(fd, iov, count) syscall handler.
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
 * @brief         Writes scattered runs through one descriptor.
 *
 * @return        The total bytes written, or a negative errno; a failed
 *                segment reports the bytes already written before it.
 * @note          The hosted world buffers in pieces — a shell prompt is
 *                one run, the answer another — and hands both over in
 *                one call so the write side never sees half a thought.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleWritev(const SyscallFrame& frame);

}  // namespace cinux::syscall
