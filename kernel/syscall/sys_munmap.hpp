/**
 * @file    sys_munmap.hpp
 * @brief   munmap: withdrawing promises and dropping the pages they held.
 * @author  Charliechen114514
 * @date    2026-10-14
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Serves munmap(addr, len).
 *
 * @param[in]     frame   The trap frame; rdi carries the stretch's
 *                        first byte, rsi the byte count.
 * @return        Zero, or a negative errno.
 * @note          The register decides first: a stretch that would split
 *                a recorded span is refused untouched, Linux ENOMEM and
 *                all. Only after the promises are withdrawn do the
 *                translations follow, each present page returning its
 *                frame through the CoW-aware release so a shared frame
 *                merely loses one sharer.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_syscall
 */
long long HandleMunmap(const SyscallFrame& frame);

}  // namespace cinux::syscall
