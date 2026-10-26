/**
 * @file    sys_close.hpp
 * @brief   Declaration of the close(fd) syscall handler.
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
 * @brief         Give a descriptor back.
 *
 * @return        What ring 3 receives: a count, a code, or a negative
 *                errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleClose(const SyscallFrame& frame);

/**
 * @brief Closes or marks a descriptor interval, optionally unsharing its table.
 * @param[in] frame First descriptor, last descriptor and flags in rdi/rsi/rdx.
 * @return Zero, or negative EINVAL or ENOMEM.
 */
long long HandleCloseRange(const SyscallFrame& frame);

}  // namespace cinux::syscall
