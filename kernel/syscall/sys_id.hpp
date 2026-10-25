/**
 * @file    sys_id.hpp
 * @brief   Declarations of the user-and-group id syscall handlers.
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
 * @brief Reads supplementary group IDs.
 * @param[in] frame Count and destination address.
 * @return Group count or negative errno.
 */
long long HandleGetgroups(const SyscallFrame& frame);

/**
 * @brief Sets the caller's supplementary group IDs.
 * @param[in] frame Count and source address.
 * @return Zero or negative errno.
 */
long long HandleSetgroups(const SyscallFrame& frame);

/**
 * @brief         getuid(): the caller's user id.
 *
 * @return        The fixed id this kernel serves.
 * @note          Every id answers root: no credential ledger exists
 *                yet, so a task is its own superuser.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleGetuid(const SyscallFrame& frame);

/**
 * @brief         getgid(): the caller's group id.
 *
 * @return        The fixed id this kernel serves.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleGetgid(const SyscallFrame& frame);

/**
 * @brief         setuid(uid): adopt a user id.
 *
 * @return        Always zero: an offer to become root, from root,
 *                never fails.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleSetuid(const SyscallFrame& frame);

/**
 * @brief         setgid(gid): adopt a group id.
 *
 * @return        Always zero, for the same reason setuid gives.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleSetgid(const SyscallFrame& frame);

/**
 * @brief         geteuid(): the effective user id.
 *
 * @return        The fixed id this kernel serves.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleGeteuid(const SyscallFrame& frame);

/**
 * @brief         getegid(): the effective group id.
 *
 * @return        The fixed id this kernel serves.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleGetegid(const SyscallFrame& frame);

}  // namespace cinux::syscall
