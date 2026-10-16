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

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         stat(path, out).
 * @param[in]     frame   The trap frame; rdi carries the path address,
 *                        relative or absolute, rsi the user address
 *                        receiving a SyscallStat.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleStat(const SyscallFrame& frame);

}  // namespace cinux::syscall
