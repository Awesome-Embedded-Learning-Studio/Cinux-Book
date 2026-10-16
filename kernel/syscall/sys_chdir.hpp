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

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         chdir(path).
 * @param[in]     frame   The trap frame; the path address, relative or
 *                        absolute, rides rdi.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleChdir(const SyscallFrame& frame);

}  // namespace cinux::syscall
