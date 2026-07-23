/**
 * @file kernel/fs/procfs_init.cpp
 * @brief ProcFS boot wiring: mount + register at /proc (F6-M2)
 *
 * Kernel-only translation unit (linked into big_kernel_common, NOT into the host
 * unit tests).  Owns the procfs::init() boot hook that constructs the ProcFs,
 * mounts it, and registers it at /proc in the VFS mount table.
 *
 * Kept separate from procfs.cpp so procfs.cpp stays free of boot I/O (kprintf)
 * -- the §14 file-gate pattern, same as devfs_init.cpp / devfs.cpp.
 *
 * Namespace: cinux::fs
 */

#include <stdint.h>

#include "kernel/fs/procfs/procfs.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::fs {
namespace {

// Boot-owned ProcFs.  Static local lives for the whole kernel run; the VFS
// mount table holds the pointer for the process lifetime, so it must not be
// destroyed.  The per-PID inode pools are members, allocated once at first
// mount().
ProcFs g_procfs;

}  // namespace

namespace procfs {

bool init() {
    if (!g_procfs.mount().ok()) {
        cinux::lib::kprintf("[PROCFS] mount failed\n");
        return false;
    }
    if (!vfs_mount_add("/proc", &g_procfs)) {
        cinux::lib::kprintf("[PROCFS] vfs_mount_add /proc failed (table full?)\n");
        return false;
    }
    cinux::lib::kprintf("[PROCFS] mounted at /proc\n");
    return true;
}

}  // namespace procfs
}  // namespace cinux::fs
