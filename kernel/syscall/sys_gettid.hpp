/**
 * @file    sys_gettid.hpp
 * @brief   Declaration of the gettid() syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         The caller's own thread identity.
 *
 * @return        The tid the scheduler handed this task at seat.
 * @note          Served by the task's pid until the thread group
 *                arrives: in a one-thread-per-process world the two
 *                words name the same thing, which is exactly why the
 *                number can land before its distinction does.
 * @since         0.8.0
 * @ingroup       kernel_syscall
 */
long long HandleGettid(const SyscallFrame& frame);

}  // namespace cinux::syscall
