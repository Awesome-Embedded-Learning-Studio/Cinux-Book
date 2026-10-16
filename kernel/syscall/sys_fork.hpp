/**
 * @file    sys_fork.hpp
 * @brief   Declaration of the fork() syscall handler.
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
 * @brief         One call, two returns: the child carries zero.
 *
 * @return        The child's pid in the parent, 0 is never returned
 *                here (the child's zero is written by its own entry
 *                tail), or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleFork(const SyscallFrame& frame);

}  // namespace cinux::syscall
