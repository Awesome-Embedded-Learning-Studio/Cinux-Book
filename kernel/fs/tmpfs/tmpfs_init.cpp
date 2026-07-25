/**
 * @file kernel/fs/tmpfs/tmpfs_init.cpp
 * @brief TmpFs boot wiring: mount + register at /tmp (F6-M4 / GCC self-host)
 *
 * Kernel-only translation unit (linked into big_kernel_common, NOT into the host
 * unit tests).  Owns the tmpfs::init() boot hook that constructs the static
 * TmpFs, mounts it, and registers it at /tmp in the VFS mount table -- the
 * writable in-memory filesystem GCC / cc1 / as / ld write intermediate *.o /
 * *.s into during a compile.
 *
 * Kept separate from tmpfs.cpp so tmpfs.cpp stays free of boot I/O (kprintf) --
 * the §14 file-gate pattern, same as devfs_init.cpp / procfs_init.cpp.
 *
 * The static g_tmpfs is registered unowned (vfs_mount_add default), so an
 * sys_umount2("/tmp") would detach it from the table but never free it (its
 * lifetime is the whole kernel run).  Runtime mounts via sys_mount use a heap
 * TmpFs and register owned, so they are reclaimed on umount.
 *
 * Namespace: cinux::fs
 */

#include <stdint.h>

#include "kernel/fs/tmpfs/tmpfs.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::fs {
namespace {

// Boot-owned TmpFs.  Static local lives for the whole kernel run; the VFS mount
// table holds the pointer for the process lifetime, so it must not be destroyed.
TmpFs g_tmpfs;

}  // namespace

namespace tmpfs {

bool init() {
    if (!g_tmpfs.mount().ok()) {
        cinux::lib::kprintf("[TMPFS] mount failed\n");
        return false;
    }
    if (!vfs_mount_add("/tmp", &g_tmpfs)) {
        cinux::lib::kprintf("[TMPFS] vfs_mount_add /tmp failed (table full?)\n");
        return false;
    }
    cinux::lib::kprintf("[TMPFS] mounted at /tmp\n");
    return true;
}

}  // namespace tmpfs
}  // namespace cinux::fs
