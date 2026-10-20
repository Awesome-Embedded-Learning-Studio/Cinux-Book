/**
 * @file    sys_readlink.hpp
 * @brief   readlink(2): what a link name stands for.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Answers one link's target path.
 *
 * @param[in]     frame   rdi the link path, rsi the buffer, rdx its
 *                        size.
 * @return        Bytes written, or a negative errno.
 * @note          One link exists in this kernel, the procfs magic
 *                name /proc/self/exe, whose target is the current
 *                task's own program; every other path answers EINVAL,
 *                the honest word for a file system without links.
 * @since         0.5.0
 * @ingroup       kernel_syscall
 */
long long HandleReadlink(const SyscallFrame& frame);

}  // namespace cinux::syscall
