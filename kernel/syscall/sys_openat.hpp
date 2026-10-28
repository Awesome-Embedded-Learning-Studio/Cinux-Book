/**
 * @file    sys_openat.hpp
 * @brief   Declarations of the directory-relative path syscall handlers.
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
 * @brief         openat(dirfd, path, flags, mode): open, as the hosted
 *                world writes it.
 *
 * @return        The new descriptor, or a negative errno.
 * @note          Absolute paths ignore dirfd. Relative paths use the
 *                open directory's recorded path, or cwd for AT_FDCWD.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleOpenat(const SyscallFrame& frame);

/**
 * @brief         newfstatat(dirfd, path, out, flags): stat, relative
 *                to a descriptor.
 *
 * @return        Zero when the record lands, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleNewfstatat(const SyscallFrame& frame);

}  // namespace cinux::syscall
