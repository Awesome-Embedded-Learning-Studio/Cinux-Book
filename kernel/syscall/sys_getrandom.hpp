/**
 * @file    sys_getrandom.hpp
 * @brief   Declaration of the getrandom(buf, len, flags) syscall handler.
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
 * @brief         Fills the caller's buffer with unpredictable bytes.
 *
 * @return        The bytes written, or a negative errno.
 * @note          The hosted runtime wants a canary seed and hash salt
 *                before any file name is touched; the mix below stirs
 *                the heartbeat and the buffer's own address through a
 *                xorshift — no hardware entropy source exists yet, and
 *                the mixing, not the secrecy, is what the early callers
 *                need.
 * @since         0.3.0
 * @ingroup kernel_syscall
 */
long long HandleGetrandom(const SyscallFrame& frame);

}  // namespace cinux::syscall
