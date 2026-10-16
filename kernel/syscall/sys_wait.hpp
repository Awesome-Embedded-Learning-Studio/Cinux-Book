/**
 * @file    sys_wait.hpp
 * @brief   Declaration of the wait4(pid, status) syscall handler.
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
 * @brief         Block until one child exits, then retire it.
 *
 * @return        The reaped child's pid, or a negative errno.
 * @note          The status word lands as the raw exit code, not the
 *                Linux wait-status encoding — the shift macros arrive
 *                with the musl station, which is also when the encoding
 *                has its first reader.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleWait4(const SyscallFrame& frame);

}  // namespace cinux::syscall
