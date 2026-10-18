/**
 * @file    sys_write.hpp
 * @brief   Declaration of the write(fd, buf, count) syscall handler.
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
 * @brief         Console out, or bytes into an open file.
 *
 * @return        What ring 3 receives: a count, a code, or a negative
 *                errno.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleWrite(const SyscallFrame& frame);

/**
 * @brief         Writes one run of bytes through one descriptor.
 *
 * @param[in]     descriptor   The fd the bytes belong to; 1 and 2 are
 *                             the console, the rest are opens.
 * @param[in]     source       Where the bytes come from, user memory.
 * @param[in]     count        How many bytes to put.
 * @return        The count written, or a negative errno.
 * @note          The single-run body write and writev share; the scatter
 *                caller adds the segments, the plain caller is one
 *                segment.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long WriteRun(unsigned long long descriptor, unsigned long long source,
                   unsigned long long count);

}  // namespace cinux::syscall
