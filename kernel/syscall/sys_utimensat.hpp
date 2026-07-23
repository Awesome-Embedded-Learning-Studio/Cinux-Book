/**
 * @file kernel/syscall/sys_utimensat.hpp
 * @brief sys_utimensat handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Set the access / modification times of the file at @p resolved_path. ext2
/// revision-0 inodes store whole seconds; nsec is accepted but truncated.
/// Returns 0 or -errno.
int64_t do_utimensat_kernel(const char* resolved_path, uint64_t atime_sec, uint32_t atime_nsec,
                            uint64_t mtime_sec, uint32_t mtime_nsec);

/// Linux utimensat(2): set times of @p path_virt from a user timespec[2].
/// @p times_virt == 0 means "now" (currently a 0 placeholder -- no wall clock
/// wired here yet). flags (AT_SYMLINK_NOFOLLOW / AT_EMPTY_PATH) are accepted
/// but ignored: lookup does not follow symlinks yet, so path is always the link
/// itself. dirfd is AT_FDCWD only (per-fd cwd is a follow-up).
int64_t sys_utimensat(uint64_t dirfd, uint64_t path_virt, uint64_t times_virt, uint64_t flags,
                      uint64_t, uint64_t);

}  // namespace cinux::syscall
