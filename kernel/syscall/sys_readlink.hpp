/**
 * @file kernel/syscall/sys_readlink.hpp
 * @brief sys_readlink handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Read the target of the symlink at @p resolved_path into @p buf (at most
/// @p buf_size bytes, not NUL-terminated). Returns bytes written (>=0) or -errno.
int64_t do_readlink_kernel(const char* resolved_path, char* buf, uint64_t buf_size);

/// Linux readlink(2): read symlink target of @p path_virt into @p buf_virt.
/// Returns the number of bytes placed in the buffer (no NUL terminator).
int64_t sys_readlink(uint64_t path_virt, uint64_t buf_virt, uint64_t buf_size, uint64_t, uint64_t,
                     uint64_t);

}  // namespace cinux::syscall
