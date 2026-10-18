/**
 * @file    sys_uname.hpp
 * @brief   Declaration of the uname(buf) syscall handler.
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
 * @brief         Names the system: kernel, node, release, machine.
 *
 * @return        Zero on success, or a negative errno.
 * @note          The hosted world asks before it decides how to behave;
 *                every field is one NUL-terminated run inside the
 *                caller's six-slot record.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleUname(const SyscallFrame& frame);

}  // namespace cinux::syscall
