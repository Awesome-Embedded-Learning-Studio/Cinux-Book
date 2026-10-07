/**
 * @file    sys_open.hpp
 * @brief   Declaration of the open(path, flags) syscall handler.
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
 * @brief         Claim a descriptor for a path, creating on O_CREAT.
 *
 * @return        What ring 3 receives: a count, a code, or a negative
 *                errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleOpen(Word path, Word flags, Word third);

}  // namespace cinux::syscall
