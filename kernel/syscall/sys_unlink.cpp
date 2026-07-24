/**
 * @file kernel/syscall/sys_unlink.cpp
 * @brief sys_unlink handler implementation
 *
 * Removes a file by resolving the parent directory through the VFS
 * and calling InodeOps::unlink() on it.
 */

#include "kernel/syscall/sys_unlink.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"  // inode_unref
#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t do_unlink_kernel(const char* resolved_path) {
    // Step 1: Resolve through the VFS mount table
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_UNLINK] No filesystem mounted for '%s'\n", resolved_path);
        return -kEnoent;
    }

    // Step 2: Split relative path into parent dir and leaf name
    cinux::fs::PathBuf parent_buf;
    const char*        leaf_name = nullptr;
    uint32_t           name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        kprintf("[SYS_UNLINK] Invalid path: '%s'\n", resolved_path);
        return -kEinval;
    }

    // Step 3: Look up the parent directory inode
    auto parent_result = fs->lookup(parent_buf);
    if (!parent_result.ok()) {
        kprintf("[SYS_UNLINK] Parent directory not found for '%s'\n", resolved_path);
        return -to_errno(parent_result.error());
    }
    cinux::fs::Inode* parent = parent_result.value();  // ref'd by lookup

    if (parent->ops == nullptr) {
        kprintf("[SYS_UNLINK] Parent inode has no ops\n");
        cinux::fs::inode_unref(parent);
        return -kEio;
    }

    // Step 4: Call unlink() on the parent directory
    auto unlink_result = parent->ops->unlink(parent, leaf_name, name_len);
    cinux::fs::inode_unref(parent);  // drop the lookup ref
    if (!unlink_result.ok()) {
        kprintf("[SYS_UNLINK] Failed to unlink '%s'\n", resolved_path);
        return -to_errno(unlink_result.error());
    }

    return 0;
}

int64_t sys_unlink(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Boundary: resolve the user path (cwd-aware), then run kernel logic.
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }
    return do_unlink_kernel(resolved.data());
}

}  // namespace cinux::syscall
