/**
 * @file    sys_yield.hpp
 * @brief   Declaration of the yield() syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

/**
 * @brief         Hand the CPU over for a turn.
 *
 * @return        What ring 3 receives: a count, a code, or a negative
 *                errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleYield(Word first, Word second, Word third);

}  // namespace cinux::syscall
