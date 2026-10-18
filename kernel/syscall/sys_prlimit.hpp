/**
 * @file    sys_prlimit.hpp
 * @brief   Declarations of the resource-and-process knob syscall handlers.
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
 * @brief         prlimit64(pid, res, new, old): resource ceilings.
 *
 * @return        Zero when the row is served, or a negative errno.
 * @note          Only the calling task's own row is reachable; a
 *                stranger pid answers -EPERM. The copy-in copy-out
 *                record is one limits row of the task itself, so the
 *                width cannot drift from the table it lands in.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandlePrlimit64(const SyscallFrame& frame);

/**
 * @brief         prctl(op, ...): process knobs, the name op served.
 *
 * @return        Zero for a served op, or a negative errno.
 * @note          PR_SET_NAME copies at most fifteen name bytes into
 *                the task's comm, PR_GET_NAME reads the whole record
 *                back; any other op answers -EINVAL.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandlePrctl(const SyscallFrame& frame);

}  // namespace cinux::syscall
