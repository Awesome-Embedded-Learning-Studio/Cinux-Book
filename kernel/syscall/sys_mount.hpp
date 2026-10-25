/**
 * @file    sys_mount.hpp
 * @brief   Declarations of the mount and umount2 syscall handlers.
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
 * @brief         Seats a fresh memory volume at a directory.
 *
 * @return        Zero, or a negative errno.
 * @note          The one filesystem type served is tmpfs; the source
 *                word is decoration the kernel does not read.
 * @since         0.4.0
 * @ingroup       kernel_syscall
 */
long long HandleMount(const SyscallFrame& frame);

/**
 * @brief         Takes one runtime tmpfs seat back.
 *
 * @return        Zero, or a negative errno.
 * @note          Only seats the mount syscall itself made come off;
 *                the boot's own mounts are not the runtime's to take
 *                apart.
 * @since         0.4.0
 * @ingroup       kernel_syscall
 */
long long HandleUmount2(const SyscallFrame& frame);

}  // namespace cinux::syscall
