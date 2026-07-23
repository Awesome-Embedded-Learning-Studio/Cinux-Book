/**
 * @file kernel/syscall/sys_stat.hpp
 * @brief sys_stat and sys_fstat handler declarations
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/fs/stat.hpp"  // P0a: do_*_kernel takes cinux::fs::stat*

namespace cinux::syscall {

/**
 * @brief Get file status by path
 *
 * Resolves the path through the VFS and fills a struct stat.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @param st_virt    User virtual address of the struct stat output buffer
 * @return 0 on success, or -1 on error
 */
int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t);

/**
 * @brief Get file status by file descriptor
 *
 * Looks up the FD in the global FD table and fills a struct stat.
 *
 * @param fd         File descriptor index
 * @param st_virt    User virtual address of the struct stat output buffer
 * @return 0 on success, or -1 on error
 */
int64_t sys_fstat(uint64_t fd, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t);

/**
 * @brief Get file status relative to a directory fd (F10-M1 batch 4)
 *
 * musl's stat()/fstat()/lstat() all route through newfstatat.  AT_FDCWD
 * resolves cwd-relative (the common case); AT_EMPTY_PATH stats @p dirfd
 * itself.  Symlink-following is implicit -- CinuxOS has no symlink support.
 */
int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_virt, uint64_t st_virt, uint64_t flags,
                       uint64_t, uint64_t);

// ============================================================
// P0a (SMAP): pure kernel-to-kernel stat logic (no user memory).
// Kernel-internal callers and tests use these; sys_* are the user boundaries.
// ============================================================

/// Stat a resolved path (already canonicalised, e.g. from path_resolve) into a
/// kernel stat buffer. Returns 0 or -errno.
int64_t do_stat_kernel(const char* resolved_path, cinux::fs::stat* kst);

/// Stat an open fd into a kernel stat buffer. Returns 0 or -errno.
int64_t do_fstat_kernel(int fd, cinux::fs::stat* kst);

}  // namespace cinux::syscall
