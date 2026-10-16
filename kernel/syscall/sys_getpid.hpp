/**
 * @file    sys_getpid.hpp
 * @brief   Declaration of the getpid() syscall handler.
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
 * @brief         The caller's own pid.
 *
 * @return        The pid the scheduler handed this task at seat.
 * @since         0.1.0
 * @ingroup kernel_syscall
 */
long long HandleGetpid(const SyscallFrame& frame);

}  // namespace cinux::syscall
