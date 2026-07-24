/**
 * @file kernel/syscall/sys_umount2.cpp
 * @brief sys_umount2 handler implementation (F6-M1)
 *
 * Thin wrapper over the ownership-aware vfs_mount_remove: detach the mount at
 * the target path.  A sys_mount-created backend (owned) is freed by the table
 * layer; a boot/static mount is detached without freeing.  @p flags (MNT_FORCE
 * etc.) accepted but ignored -- Cinux does not yet model forced unmount.
 */

#include "kernel/syscall/sys_umount2.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/path.hpp"            // PathBuf
#include "kernel/fs/vfs_mount.hpp"       // vfs_mount_remove
#include "kernel/syscall/path_util.hpp"  // resolve_user_path

namespace cinux::syscall {

int64_t do_umount2_kernel(const char* target, [[maybe_unused]] uint64_t flags) {
    // MNT_FORCE / MNT_DETACH / MNT_EXPIRE not yet modelled

    if (target == nullptr || target[0] == '\0') {
        return -kEinval;
    }
    if (!cinux::fs::vfs_mount_remove(target)) {
        return -kEnoent;  // nothing mounted at that path
    }
    return 0;
}

int64_t sys_umount2(uint64_t target_virt, uint64_t flags, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::PathBuf target;
    if (!resolve_user_path(target_virt, target.data())) {
        return -kEfault;
    }
    return do_umount2_kernel(target.data(), flags);
}

}  // namespace cinux::syscall
