/**
 * @file kernel/syscall/sys_symlink.hpp
 * @brief sys_symlink handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Create a symbolic link named by @p resolved_linkpath pointing at @p target
/// (a literal string, not canonicalised). Returns 0 or -errno.
int64_t do_symlink_kernel(const char* target, const char* resolved_linkpath);

/// Linux symlink(2): create link @p linkpath_virt -> @p target_virt.
int64_t sys_symlink(uint64_t target_virt, uint64_t linkpath_virt, uint64_t, uint64_t, uint64_t,
                    uint64_t);

}  // namespace cinux::syscall
