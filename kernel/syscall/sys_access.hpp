/**
 * @file    sys_access.hpp
 * @brief   Declaration of the access(path, mode) syscall handler.
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
 * @brief         Does the path answer at all.
 *
 * @return        Zero when the path resolves, a negative errno when it
 *                does not.
 * @note          The hosted shell hunts executables by poking names;
 *                with every task its own superuser, existence is the
 *                whole of permission, so the mode bits ride along
 *                unexamined.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleAccess(const SyscallFrame& frame);

}  // namespace cinux::syscall
