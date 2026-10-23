/**
 * @file    uaccess.hpp
 * @brief   The one door kernel code crosses to touch user memory.
 *
 * Every byte the kernel moves between itself and a ring-3 address goes
 * through these two faces. The range half answers before any access
 * happens; the copy half runs inside the SMAP window on machines that
 * promised one, so the window never spans anything that blocks or
 * schedules. Until the exception table arrives, a copy from an
 * unmapped page faults like any other kernel access — the door does
 * not yet know how to apologize. The announcement only names the
 * faces; the door's body lives in its own translation unit, and the
 * hosted tests carry a shadow of it beside the range checks.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall::uaccess {

/**
 * @brief         Records whether this machine serves SMAP windows.
 *
 * @param[in]     supported   The CPUID answer for SMAP, taken once at
 *                            boot and again in each test world.
 * @return        None.
 * @note          stac and clac fault with #UD on a processor whose
 *                CPUID answer lacks SMAP, so the copies reach for the
 *                window only when this bit says the machine has one.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
void ArmWindows(bool supported);

/**
 * @brief         Moves bytes from a user address into kernel memory.
 *
 * @param[out]    destination   Kernel destination; caller owns the storage.
 * @param[in]     source        User source address as the syscall carried it.
 * @param[in]     bytes         How many bytes to move; zero succeeds without
 *                              touching anything.
 * @return        True when the range answered and the bytes moved;
 *                              false when the range refused.
 * @note          The copy runs inside the SMAP window when armed. The
 *                              caller must not hold this window across
 *                              anything that can block or schedule — take
 *                              what is needed, give it back, then wait.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
[[nodiscard]] bool CopyFromUser(void* destination, Word source, Word bytes);

/**
 * @brief         Moves bytes from kernel memory to a user address.
 *
 * @param[in]     destination   User destination address as the syscall
 *                              carried it.
 * @param[in]     source        Kernel source; caller owns the storage.
 * @param[in]     bytes         How many bytes to move; zero succeeds without
 *                              touching anything.
 * @return        True when the range answered and the bytes moved;
 *                              false when the range refused.
 * @note          Same window discipline as CopyFromUser: the window
 *                              opens for the move and closes before the
 *                              caller proceeds.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
[[nodiscard]] bool CopyToUser(Word destination, const void* source, Word bytes);

}  // namespace cinux::syscall::uaccess
