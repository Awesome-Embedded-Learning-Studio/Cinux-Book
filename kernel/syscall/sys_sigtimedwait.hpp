/**
 * @file    sys_sigtimedwait.hpp
 * @brief   Declaration of the rt_sigtimedwait syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-25
 * @version 0.1
 * @since   0.4.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Waits for one of a set of signals, with a timeout.
 *
 * @return        Always -EAGAIN: the wait is answered try-again rather
 *                than parked, so a caller that loops on it keeps making
 *                progress instead of sleeping forever.
 * @since         0.4.0
 * @ingroup       kernel_syscall
 */
long long HandleRtSigtimedwait(const SyscallFrame& frame);

}  // namespace cinux::syscall
