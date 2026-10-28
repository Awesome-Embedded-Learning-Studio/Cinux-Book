/**
 * @file    sys_sync.hpp
 * @brief   Synchronize pending filesystem data and metadata.
 * @author  Charliechen114514
 * @date    2026-10-28
 * @version 0.1
 * @since   0.3.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Wait for the persistent filesystem cache and device to synchronize.
 * @param[in] frame Unused; sync takes no arguments.
 * @return Zero on success, or -kEio when synchronization fails.
 */
long long HandleSync(const SyscallFrame& frame);

}
