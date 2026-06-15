/**
 * @file kernel/syscall/sys_rmdir.cpp
 * @brief sys_rmdir handler implementation
 *
 * Removes an empty directory by resolving the parent directory through
 * the VFS and calling InodeOps::unlink() on it.  The backend filesystem
 * (e.g. ext2) is responsible for verifying that the target is an empty
 * directory before removing it.
 */

#include "kernel/syscall/sys_rmdir.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t sys_rmdir(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -kEfault;
    }

    // Step 2: Resolve through the VFS mount table
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_RMDIR] No filesystem mounted for '%s'\n", resolved);
        return -kEnoent;
    }

    // Step 3: Split relative path into parent dir and leaf name
    char        parent_buf[cinux::fs::PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        kprintf("[SYS_RMDIR] Invalid path: '%s'\n", resolved);
        return -kEinval;
    }

    // Step 4: Look up the parent directory inode
    auto parent_result = fs->lookup(parent_buf);
    if (!parent_result.ok()) {
        kprintf("[SYS_RMDIR] Parent directory not found for '%s'\n", resolved);
        return -to_errno(parent_result.error());
    }
    cinux::fs::Inode* parent = parent_result.value();

    if (parent->ops == nullptr) {
        kprintf("[SYS_RMDIR] Parent inode has no ops\n");
        return -kEio;
    }

    // Step 5: Look up the target to verify it's an empty directory
    auto target_result = fs->lookup(rel_path);
    if (!target_result.ok()) {
        kprintf("[SYS_RMDIR] '%s' not found\n", resolved);
        return -to_errno(target_result.error());
    }
    cinux::fs::Inode* target = target_result.value();
    if (target->type != cinux::fs::InodeType::Directory) {
        kprintf("[SYS_RMDIR] '%s' is not a directory\n", resolved);
        return -kEnotdir;
    }
    // Check directory is empty: try readdir index 2 (index 0=".", 1="..")
    // If there's a 3rd entry, the directory is not empty
    if (target->ops != nullptr) {
        char check_name[16];
        auto dir_check = target->ops->readdir(target, 2, check_name, sizeof(check_name));
        if (!dir_check.ok()) {
            kprintf("[SYS_RMDIR] failed to check whether '%s' is empty\n", resolved);
            return -to_errno(dir_check.error());
        }
        if (dir_check.value() > 0) {
            kprintf("[SYS_RMDIR] '%s' is not empty\n", resolved);
            return -kEnotempty;
        }
    }

    // Step 6: Call unlink() on the parent directory
    auto unlink_result = parent->ops->unlink(parent, leaf_name, name_len);
    if (!unlink_result.ok()) {
        kprintf("[SYS_RMDIR] Failed to rmdir '%s'\n", resolved);
        return -to_errno(unlink_result.error());
    }

    return 0;
}

}  // namespace cinux::syscall
