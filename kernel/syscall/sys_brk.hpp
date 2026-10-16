/**
 * @file    sys_brk.hpp
 * @brief   brk: moving the break, Linux's return-shape and all.
 * @author  Charliechen114514
 * @date    2026-10-14
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Serves brk(target).
 *
 * @param[in]     frame   The trap frame; rdi carries the break wanted,
 *                        0 only asks.
 * @return        The break after the call — the new one on success, the
 *                standing one on refusal. brk speaks in addresses, not
 *                in errnos; a negative return would be a legal address,
 *                so failure reads as "the break did not move".
 * @note          No page is mapped or unmapped: the cursor moves and the
 *                heap span's end moves with it, and pages inside the
 *                span arrive when their faults ask.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_syscall
 */
long long HandleBrk(const SyscallFrame& frame);

}  // namespace cinux::syscall
