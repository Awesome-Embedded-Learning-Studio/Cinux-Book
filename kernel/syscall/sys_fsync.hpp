/**
 * @file sys_fsync.hpp
 * @brief Persistent file synchronization.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {
/**
 * @brief Flushes cached file data and metadata to the backing device.
 * @param[in] frame The descriptor in rdi.
 * @return Zero on success, or negative EBADF, EINVAL or EIO.
 * @note The current persistent filesystem flushes its complete cache.
 */
long long HandleFsync(const SyscallFrame& frame);
}
