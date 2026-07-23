/**
 * @file kernel/syscall/sys_mknod.hpp
 * @brief sys_mknod handler declaration (F8-M2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Create a filesystem node (FIFO this milestone)
 *
 * Linux SYS_mknod(path, mode, dev).  Only S_IFIFO is implemented: it registers
 * a named FIFO (mkfifo is mknod with S_IFIFO).  Other node types (char/block
 * devices) return -ENOSYS.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @param mode       File mode (type + permissions); S_IFIFO selects FIFO creation
 * @param dev        Device number (ignored for FIFO)
 * @return 0 on success, or -errno
 */
int64_t sys_mknod(uint64_t path_virt, uint64_t mode, uint64_t dev, uint64_t, uint64_t, uint64_t);

/// Pure kernel-to-kernel mknod on an already-resolved path. Returns 0 or -errno.
int64_t do_mknod_kernel(const char* resolved_path, uint32_t mode);

}  // namespace cinux::syscall
