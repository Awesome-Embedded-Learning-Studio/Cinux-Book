/**
 * @file kernel/syscall/sys_link.hpp
 * @brief sys_link handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Create a hard link at @p resolved_newpath referring to the inode at
/// @p resolved_oldpath (same filesystem only; cross-fs EXDEV is a follow-up).
/// Returns 0 or -errno.
int64_t do_link_kernel(const char* resolved_oldpath, const char* resolved_newpath);

/// Linux link(2): create a hard link @p newpath_virt -> @p oldpath_virt.
int64_t sys_link(uint64_t oldpath_virt, uint64_t newpath_virt, uint64_t, uint64_t, uint64_t,
                 uint64_t);

}  // namespace cinux::syscall
