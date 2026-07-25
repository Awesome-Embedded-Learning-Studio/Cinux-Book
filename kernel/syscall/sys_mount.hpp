/**
 * @file kernel/syscall/sys_mount.hpp
 * @brief sys_mount (Linux 165) handler declaration (F6-M1)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Kernel-internal mount.  Constructs a backend from @p fstype (currently only
/// "tmpfs"), mount()s it, and registers it at the absolute @p target path.
/// @p source is unused (tmpfs has no backing device); accepted for parity with
/// the Linux ABI and future source-bearing fstypes.  @p flags (MS_*) ignored.
/// The new backend is heap-allocated and registered as owned, so a later
/// do_umount2_kernel frees it.  Returns 0 or -errno.
int64_t do_mount_kernel(const char* source, const char* target, const char* fstype, uint64_t flags);

/// Linux mount(2): @p source_virt / @p target_virt / @p fstype_virt are user
/// pointers; @p data is mount-options (ignored).  Returns 0 or -errno.
int64_t sys_mount(uint64_t source_virt, uint64_t target_virt, uint64_t fstype_virt, uint64_t flags,
                  uint64_t data, uint64_t);

}  // namespace cinux::syscall
