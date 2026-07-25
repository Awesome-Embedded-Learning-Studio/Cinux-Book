/**
 * @file kernel/syscall/sys_open.hpp
 * @brief sys_open handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Open a file and return a file descriptor
 *
 * Resolves the path through the VFS mount table, looks up the
 * corresponding Inode in the backend filesystem, and allocates
 * a file descriptor in the global FDTable.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @param flags      Access mode (0=RDONLY, 1=WRONLY, 2=RDWR)
 * @return Non-negative file descriptor on success, or -1 on error
 */
int64_t sys_open(uint64_t path_virt, uint64_t flags, uint64_t mode, uint64_t, uint64_t, uint64_t);

/**
 * @brief Open relative to a directory fd (F10-M1 batch 4)
 *
 * musl's open()/fopen() always go through openat.  @p dirfd is AT_FDCWD
 * (-100) for cwd-relative opens (the only case musl uses); other dirfds
 * fall back to cwd as a documented limitation until per-fd paths are
 * tracked.  O_CREAT creates the file when it is missing.
 */
int64_t sys_openat(uint64_t dirfd, uint64_t path_virt, uint64_t flags, uint64_t mode, uint64_t,
                   uint64_t);

// ============================================================
// P0g (SMAP): pure kernel-to-kernel open logic (no user memory).
// Kernel-internal callers and tests use these; sys_open / sys_openat are the
// user boundaries.
// ============================================================

/// Open an existing file at an already-resolved path and allocate an fd.
/// @p flags is the raw access mode (0=RDONLY, 1=WRONLY, 2=RDWR). Returns fd or
/// -errno.
int64_t do_open_kernel(const char* resolved_path, uint64_t flags);

/// Open (creating if O_CREAT) at an already-resolved path and allocate an fd.
/// @p flags is the raw Linux open() flag word (access mode + O_CREAT ...).
/// @p mode is the requested permission bits for a newly created file.
/// Returns fd or -errno.
int64_t do_openat_kernel(const char* resolved_path, uint64_t flags, uint64_t mode = 0);

}  // namespace cinux::syscall
