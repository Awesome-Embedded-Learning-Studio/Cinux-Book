/**
 * @file    sys_stat.hpp
 * @brief   stat(path, out): one node's metadata for ring 3.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

/**
 * @brief         stat(path, out).
 * @param[in]     path    User address of the path, relative or absolute.
 * @param[in]     out     User address receiving a SyscallStat.
 * @param[in]     third   Unused.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleStat(Word path, Word out, Word third);

}  // namespace cinux::syscall
