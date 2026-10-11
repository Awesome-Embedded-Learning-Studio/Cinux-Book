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

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

/**
 * @brief         reboot(): cut the power.
 * @param[in]     first   Unused.
 * @param[in]     second  Unused.
 * @param[in]     third   Unused.
 * @return        Zero when the write went out; -kEnosys when the
 *                firmware publishes no ACPI tables to walk.
 * @note          The Linux number carries a four-argument magic
 *                protocol; the teaching face takes none of it — the
 *                one command is power off.
 * @since         0.1.0
 * @ingroup kernel_syscall
 */
long long HandleReboot(Word first, Word second, Word third);

}  // namespace cinux::syscall
