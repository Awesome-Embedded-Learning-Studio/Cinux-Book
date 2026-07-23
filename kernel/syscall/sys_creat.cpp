/**
 * @file kernel/syscall/sys_creat.cpp
 * @brief sys_creat handler implementation
 *
 * Creates a new regular file by resolving the parent directory through
 * the VFS and calling InodeOps::create() on it.
 */

#include "kernel/syscall/sys_creat.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t do_creat_kernel(const char* resolved_path) {
    // Step 1: Resolve through the VFS mount table
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_CREAT] No filesystem mounted for '%s'\n", resolved_path);
        return -kEnoent;
    }

    // Step 2: Split relative path into parent dir and leaf name
    cinux::fs::PathBuf parent_buf;
    const char*        leaf_name = nullptr;
    uint32_t           name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        kprintf("[SYS_CREAT] Invalid path: '%s'\n", resolved_path);
        return -kEinval;
    }

    // Step 3: Look up the parent directory inode
    auto parent_result = fs->lookup(parent_buf);
    if (!parent_result.ok()) {
        kprintf("[SYS_CREAT] Parent directory not found for '%s'\n", resolved_path);
        return -to_errno(parent_result.error());
    }
    cinux::fs::Inode* parent = parent_result.value();

    if (parent->ops == nullptr) {
        kprintf("[SYS_CREAT] Parent inode has no ops\n");
        return -kEio;
    }

    // Step 4: Try to create the file
    auto create_result = parent->ops->create(parent, leaf_name, name_len);
    if (create_result.ok()) {
        return 0;  // new file created successfully
    }

    // Step 5: create() returned nullptr -- file may already exist.
    // Truncate it to 0 bytes (POSIX creat semantics).
    auto existing_result = fs->lookup(rel_path);
    if (!existing_result.ok() || existing_result.value()->ops == nullptr) {
        kprintf("[SYS_CREAT] Failed to create or truncate '%s'\n", resolved_path);
        if (!existing_result.ok()) {
            return -to_errno(existing_result.error());
        }
        return -kEio;
    }
    cinux::fs::Inode* existing = existing_result.value();

    if (existing->size > 0) {
        existing->size = 0;
    }

    return 0;
}

int64_t sys_creat(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Boundary: resolve the user path (cwd-aware), then run kernel logic.
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }
    return do_creat_kernel(resolved.data());
}

}  // namespace cinux::syscall
