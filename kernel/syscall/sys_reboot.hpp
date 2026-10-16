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
 * @brief         reboot(): cut the power.
 * @param[in]     frame   Unread; the one command needs no argument.
 * @return        Zero when the write went out; -kEnosys when the
 *                firmware publishes no ACPI tables to walk.
 * @note          The Linux number carries a four-argument magic
 *                protocol; the teaching face takes none of it — the
 *                one command is power off.
 * @since         0.1.0
 * @ingroup kernel_syscall
 */
long long HandleReboot(const SyscallFrame& frame);

}  // namespace cinux::syscall
