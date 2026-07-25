/**
 * @file kernel/syscall/sys_access.hpp
 * @brief sys_access (Linux 21) handler declaration (F6 / GCC self-host batch 3a)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Kernel-internal access(2): check the current task's permission on the file at
/// @p resolved_path for @p mode (F_OK=0 / R_OK=4 / W_OK=2 / X_OK=1, OR'd).  Root
/// bypasses R/W; X requires at least one execute bit.  Returns 0 or -errno.
int64_t do_access_kernel(const char* resolved_path, uint32_t mode);

/// Linux access(2): @p path_virt is a user pointer.
int64_t sys_access(uint64_t path_virt, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
