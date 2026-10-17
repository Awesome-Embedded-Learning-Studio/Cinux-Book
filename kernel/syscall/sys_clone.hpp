/**
 * @file    sys_clone.hpp
 * @brief   Declaration of the clone() syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.1
 * @since   0.8.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Fork with a say in what stays shared.
 *
 * @param[in]     frame   The entry frame: rdi carries the flags, rsi the
 *                        child's first user stack pointer, r8 the
 *                        thread pointer for CLONE_SETTLS.
 * @return        The child's pid in the parent, or a negative errno.
 * @note          The child wakes at the same syscall return tail the
 *                fork child does, with rax zero and the caller-given
 *                stack under it; it is a thread of the maker's group
 *                under CLONE_THREAD, so getpid there answers the
 *                maker's tgid.
 * @since         0.8.0
 * @ingroup       kernel_syscall
 */
long long HandleClone(const SyscallFrame& frame);

}  // namespace cinux::syscall
