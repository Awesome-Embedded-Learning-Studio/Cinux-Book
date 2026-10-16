/**
 * @file    sys_exec.hpp
 * @brief   Declaration of the execve(path) syscall handler.
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
 * @brief         Replace this task's world with a program from the volume.
 *
 * @return        Never on success — ring 3 resumes at the new entry;
 *                a negative errno when anything refused along the way.
 * @note          The teaching shape: one path argument, no argv or
 *                envp, the first act's honest cut, waiting for the
 *                musl station to make them real.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleExecve(const SyscallFrame& frame);

}  // namespace cinux::syscall
