/**
 * @file    sys_sysinfo.hpp
 * @brief   sysinfo(2): the machine's vital signs in one record.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Fills one sysinfo record.
 *
 * @param[in]     frame   rdi the record's address.
 * @return        Zero, or a negative errno.
 * @note          The memory words carry the page manager's two counts
 *                scaled to bytes, procs counts living process leaders,
 *                and the unit word is one; uptime and loads stay zero,
 *                the honest word for a kernel with no load meter yet.
 * @since         0.5.0
 * @ingroup       kernel_syscall
 */
long long HandleSysinfo(const SyscallFrame& frame);

}  // namespace cinux::syscall
