/**
 * @file    sys_set_tid_address.hpp
 * @brief   Declaration of the set_tid_address() syscall handler.
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
 * @brief         Name the user word the kernel clears when this task
 *                dies.
 *
 * @return        The caller's tid, as the pthread-family protocol
 *                expects even though the clearing itself comes later.
 * @since         0.8.0
 * @ingroup       kernel_syscall
 */
long long HandleSetTidAddress(const SyscallFrame& frame);

}  // namespace cinux::syscall
