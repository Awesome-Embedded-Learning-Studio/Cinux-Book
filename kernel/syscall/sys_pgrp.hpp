/**
 * @file    sys_pgrp.hpp
 * @brief   Declarations of the process-group and session syscall handlers.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Move a task into a process group.
 * @param[in]     frame   rdi the target pid (zero means the caller),
 *                        rsi the group to join (zero means the
 *                        target's own pid).
 * @return        Zero on success, or a negative errno naming why the
 *                move was refused.
 * @note          POSIX setpgid: the caller may move only itself or
 *                its own child, and Linux adds a third refusal — a
 *                child that has already exec'd stays put. v2 carries
 *                no exec'd flag, so that one refusal is simply absent;
 *                the caller discipline (shell forks, then setpgid,
 *                then exec) never reaches it. The move is legal when
 *                the new group is the target's own pid or an existing
 *                group in the target's session.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleSetpgid(const SyscallFrame& frame);

/**
 * @brief         Give the caller a session of its own.
 * @param[in]     frame   Unused.
 * @return        The new session id (the caller's pid), or a negative
 *                errno when the caller already leads a group.
 * @note          POSIX setsid: refused with EPERM when the caller is
 *                already a process-group leader, since leading a
 *                group in the old session would make the new session
 *                indistinguishable from a move within the old one.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleSetsid(const SyscallFrame& frame);

/**
 * @brief         Read a task's process group.
 * @param[in]     frame   rdi the target pid (zero means the caller).
 * @return        The group id, or a negative errno when no such task.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleGetpgid(const SyscallFrame& frame);

/** @brief Reports the caller's process group. @param[in] frame Unused arguments. @return Group
 * identity. */
long long HandleGetpgrp(const SyscallFrame& frame);

/**
 * @brief         Read a task's session id.
 * @param[in]     frame   rdi the target pid (zero means the caller).
 * @return        The session id, or a negative errno when no such
 *                task.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long HandleGetsid(const SyscallFrame& frame);

}  // namespace cinux::syscall
