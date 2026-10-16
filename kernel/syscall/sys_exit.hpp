/**
 * @file    sys_exit.hpp
 * @brief   Declaration of the exit(code) syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         End the calling task; never returns.
 *
 * @return        What ring 3 receives: a count, a code, or a negative
 *                errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
[[noreturn]] long long HandleExit(const SyscallFrame& frame);

}  // namespace cinux::syscall
