/**
 * @file kernel/syscall/sys_mkdir.cpp
 * @brief sys_mkdir handler implementation
 *
 * Creates a new directory by resolving the parent directory through
 * the VFS and calling InodeOps::mkdir() on it.
 */

#include "kernel/syscall/sys_mkdir.hpp"

#include <stdint.h>

#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t sys_mkdir(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }

    // Step 2: Resolve through the VFS mount table
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_MKDIR] No filesystem mounted for '%s'\n", resolved);
        return -1;
    }

    // Step 3: Split relative path into parent dir and leaf name
    char        parent_buf[cinux::fs::PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        kprintf("[SYS_MKDIR] Invalid path: '%s'\n", resolved);
        return -1;
    }

    // Step 4: Look up the parent directory inode
    cinux::fs::Inode* parent = fs->lookup(parent_buf);

    if (parent == nullptr) {
        kprintf("[SYS_MKDIR] Parent directory not found for '%s'\n", resolved);
        return -1;
    }

    if (parent->ops == nullptr) {
        kprintf("[SYS_MKDIR] Parent inode has no ops\n");
        return -1;
    }

    // Step 5: Call mkdir() on the parent directory
    cinux::fs::Inode* new_inode = parent->ops->mkdir(parent, leaf_name, name_len);

    if (new_inode == nullptr) {
        kprintf("[SYS_MKDIR] Failed to mkdir '%s'\n", resolved);
        return -1;
    }

    return 0;
}

}  // namespace cinux::syscall
