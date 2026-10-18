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
 * @note          The dirfd seat takes AT_FDCWD in both its signed
 *                spellings; the flag word rides the open path
 *                untouched, close-on-exec included.
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
