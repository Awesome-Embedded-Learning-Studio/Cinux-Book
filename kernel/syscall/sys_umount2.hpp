/**
 * @file kernel/syscall/sys_umount2.hpp
 * @brief sys_umount2 (Linux 166) handler declaration (F6-M1)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Kernel-internal umount.  Detaches whatever is mounted at the absolute @p
/// target path; vfs_mount_remove frees the backend iff it was registered owned
/// (sys_mount-created).  A boot/static mount (e.g. the /tmp wired by tmpfs::init)
/// is detached but its static object is left alone.  @p flags (MNT_FORCE, ...)
/// ignored.  Returns 0 or -errno.
int64_t do_umount2_kernel(const char* target, uint64_t flags);

/// Linux umount2(2): @p target_virt is a user pointer.  Returns 0 or -errno.
int64_t sys_umount2(uint64_t target_virt, uint64_t flags, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
