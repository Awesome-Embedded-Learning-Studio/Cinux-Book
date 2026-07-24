/**
 * @file kernel/syscall/sys_pread64.hpp
 * @brief sys_pread64 (Linux syscall 17) -- positioned read, B4-B2
 *
 * glibc's ldso uses pread64 to precision-read ELF segments/notes without
 * disturbing the fd's file offset. Like sys_read but the @p offset argument is
 * used directly and file->offset is NOT advanced (POSIX pread semantics).
 *
 * Namespace: cinux::syscall
 */
#pragma once

#include <cstdint>

namespace cinux::syscall {

/// Pure kernel-to-kernel positioned read. Reads @p count bytes at @p offset
/// from fd's inode; file->offset is NOT changed. Returns bytes read or -errno.
int64_t do_pread64_kernel(int fd, void* kbuf, uint64_t count, uint64_t offset);

/// User-boundary pread64: staging buffer + do_pread64_kernel + copy_to_user.
int64_t sys_pread64(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t offset, uint64_t,
                    uint64_t);

}  // namespace cinux::syscall
