/**
 * @file    sys_pread.hpp
 * @brief   Declaration of the pread64(fd, buf, count, offset) syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-25
 * @version 0.1
 * @since   0.4.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Bytes from one open, read at a position that is not the cursor.
 *
 * @return        What ring 3 receives: a count, or a negative errno.
 * @note          The fourth ABI argument rides r10, and the open's own
 *                cursor stays where it was: a positioned read leaves
 *                the descriptor untouched.
 * @since         0.4.0
 * @ingroup       kernel_syscall
 */
long long HandlePread(const SyscallFrame& frame);

}  // namespace cinux::syscall
