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

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Claim a descriptor for a path, creating on O_CREAT.
 *
 * @return        What ring 3 receives: a count, a code, or a negative
 *                errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleOpen(const SyscallFrame& frame);

/**
 * @brief         Open an already folded kernel path.
 * @return        A descriptor, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long OpenPath(const char* path, unsigned long long flags, unsigned long long mode);
/**
 * @brief         Opens one relative name through a held directory's backend.
 * @param[in]     descriptor  The held directory's open descriptor.
 * @param[in]     relative    The name relative to that descriptor.
 * @param[in]     flags       The open flags.
 * @param[in]     mode        Creation permissions.
 * @return        The descriptor, or a negative errno; kEopnotsupp asks
 *                the caller to fall back to string folding.
 * @since         0.9.0
 * @ingroup       kernel_syscall
 */
long long OpenPathAt(unsigned long long descriptor, const char* relative, unsigned long long flags,
                     unsigned long long mode);

/**
 * @brief         Create or truncate a writable file through the open path.
 * @return        The descriptor, or a negative errno.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleCreat(const SyscallFrame& frame);

}  // namespace cinux::syscall
