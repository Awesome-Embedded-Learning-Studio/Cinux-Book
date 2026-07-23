/**
 * @file kernel/syscall/sys_chmod.hpp
 * @brief sys_chmod handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Change the permission bits of the file at @p resolved_path. Only the low 12
/// bits of @p mode take effect; the type bits are preserved by the backend.
/// Returns 0 or -errno.
int64_t do_chmod_kernel(const char* resolved_path, uint32_t mode);

/// Linux chmod(2): change mode of @p path_virt.
int64_t sys_chmod(uint64_t path_virt, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
