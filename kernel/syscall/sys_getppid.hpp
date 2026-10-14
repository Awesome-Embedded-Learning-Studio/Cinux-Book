/**
 * @file    sys_getppid.hpp
 * @brief   Declaration of the getppid() syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

/**
 * @brief         Who the caller may be waited on by.
 *
 * @return        The parent's pid, zero for a parentless task.
 * @since         0.1.0
 * @ingroup kernel_syscall
 */
long long HandleGetppid(Word first, Word second, Word third);

}  // namespace cinux::syscall
