/**
 * @file    sys_reboot.hpp
 * @brief   reboot(): power the machine off through ACPI.
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
 * @brief         Validate reboot authorization, synchronize, and power off.
 * @param[in]     frame   Linux reboot magic words and power-off command.
 * @return        Zero on success; -kEperm for a non-root caller,
 *                -kEinval for invalid words, -kEnosys without ACPI,
 *                or -kEio when synchronization fails.
 * @note          A failed synchronization leaves the machine running.
 * @since         0.1.0
 * @ingroup kernel_syscall
 */
long long HandleReboot(const SyscallFrame& frame);

}  // namespace cinux::syscall
