/**
 * @file    sys_chdir.hpp
 * @brief   chdir(path): move the caller's working directory.
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
 * @brief         chdir(path).
 * @param[in]     path    User address of the directory path, relative
 *                        or absolute.
 * @param[in]     second  Unused.
 * @param[in]     third   Unused.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleChdir(Word path, Word second, Word third);

}  // namespace cinux::syscall
