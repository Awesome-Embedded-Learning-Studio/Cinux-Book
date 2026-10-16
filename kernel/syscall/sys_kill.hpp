/**
 * @file    sys_kill.hpp
 * @brief   Declaration of the kill() syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Deliver one signal to a task, a group, or everyone.
 * @param[in]     frame   rdi the target pid, rsi the signal (zero asks
 *                        only whether the target exists). Above zero
 *                        names one task; zero names every task in the
 *                        caller's own group; -1 names every other task
 *                        on the roll; below -1 names the group at
 *                        -pid.
 * @return        Zero when at least one target heard the call, or a
 *                negative errno naming why nobody did.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleKill(const SyscallFrame& frame);

}  // namespace cinux::syscall
