/**
 * @file    sys_lseek.hpp
 * @brief   Declaration of the lseek(fd, offset, whence) syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-25
 * @version 0.1
 * @since   0.8.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Moves one open's shared cursor.
 * @param[in]     frame  rdi holds the descriptor, rsi the offset,
 *                       rdx the whence code.
 * @return        What ring 3 receives: the landing position, a code,
 *                or a negative errno.
 * @note          The whence codes follow the ABI: zero anchors at the
 *                first byte, one at the cursor, two past the final
 *                byte the stat face reports; a landing below zero is
 *                refused rather than wrapped. Whence two on a backend
 *                without a stat face meets ESPIPE, the stream it is.
 * @since         0.8.0
 * @ingroup       kernel_syscall
 */
long long HandleLseek(const SyscallFrame& frame);

}  // namespace cinux::syscall
