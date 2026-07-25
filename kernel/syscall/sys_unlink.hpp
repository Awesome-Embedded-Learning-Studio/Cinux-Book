/**
 * @file kernel/syscall/sys_unlink.hpp
 * @brief sys_unlink handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Remove a file (or directory entry) at the given path
 *
 * Resolves the path through the VFS mount table, splits it into
 * parent directory and leaf name, then invokes InodeOps::unlink()
 * on the parent directory.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @return 0 on success, or -1 on error
 */
int64_t sys_unlink(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// ============================================================
// P0g (SMAP): pure kernel-to-kernel unlink logic (no user memory).
// Kernel-internal callers and tests use this; sys_unlink is the user boundary.
// ============================================================

/// Remove a file entry at an already-resolved (canonicalised) path. Returns
/// 0 or -errno.
int64_t do_unlink_kernel(const char* resolved_path);

}  // namespace cinux::syscall
