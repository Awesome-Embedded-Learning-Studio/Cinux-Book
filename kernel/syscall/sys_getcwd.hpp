/**
 * @file    sys_getcwd.hpp
 * @brief   getcwd(buf, size): the caller's working directory.
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
 * @brief         getcwd(buf, size).
 * @param[in]     out    User address receiving the NUL-terminated path.
 * @param[in]     size   Bytes writable at out, NUL included.
 * @param[in]     third  Unused.
 * @return        Bytes written without the NUL, or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleGetcwd(Word out, Word size, Word third);

}  // namespace cinux::syscall
