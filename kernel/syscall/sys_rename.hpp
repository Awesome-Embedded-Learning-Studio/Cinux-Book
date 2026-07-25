/**
 * @file kernel/syscall/sys_rename.hpp
 * @brief sys_rename handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Rename @p resolved_oldpath to @p resolved_newpath (same filesystem; the two
/// directories may be the same). Returns 0 or -errno.
int64_t do_rename_kernel(const char* resolved_oldpath, const char* resolved_newpath);

/// Linux rename(2): rename @p oldpath_virt -> @p newpath_virt.
int64_t sys_rename(uint64_t oldpath_virt, uint64_t newpath_virt, uint64_t, uint64_t, uint64_t,
                   uint64_t);

}  // namespace cinux::syscall
