/**
 * @file kernel/syscall/sys_getdents64.hpp
 * @brief sys_getdents64 (Linux 217) handler declaration -- F-ECO batch 1.
 *
 * musl opendir/readdir call getdents64 (217), NOT the legacy getdents (78).
 * Fills a real linux_dirent64 layout (the old sys_getdents only copied the
 * entry NAME -- a simplified shape musl cannot parse).
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// User-boundary handler: read directory entries for @p fd into the user
/// buffer @p buf_virt as a sequence of linux_dirent64 records. Returns the
/// total bytes written, 0 at end-of-directory, or -errno.
int64_t sys_getdents64(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t,
                       uint64_t);

}  // namespace cinux::syscall
