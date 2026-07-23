/**
 * @file kernel/syscall/sys_rename.cpp
 * @brief sys_rename handler (F-ECO batch 2)
 *
 * Resolves both parent directories, then delegates to InodeOps::rename() on the
 * source parent (remove old entry, add new entry in the destination parent).
 */

#include "kernel/syscall/sys_rename.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {
using cinux::lib::kprintf;
}

int64_t do_rename_kernel(const char* resolved_oldpath, const char* resolved_newpath) {
    // Source parent + leaf.
    const char*            rel_old = nullptr;
    cinux::fs::FileSystem* fs      = cinux::fs::vfs_resolve(resolved_oldpath, &rel_old);
    if (fs == nullptr) {
        kprintf("[SYS_RENAME] No filesystem mounted for '%s'\n", resolved_oldpath);
        return -kEnoent;
    }
    cinux::fs::PathBuf old_parent_buf;
    const char*        old_leaf = nullptr;
    uint32_t           old_len  = 0;
    if (!split_pathname(rel_old, old_parent_buf, &old_leaf, &old_len)) {
        return -kEinval;
    }
    auto old_parent_result = fs->lookup(old_parent_buf);
    if (!old_parent_result.ok()) {
        return -to_errno(old_parent_result.error());
    }
    cinux::fs::Inode* old_parent = old_parent_result.value();
    if (old_parent == nullptr || old_parent->ops == nullptr) {
        return -kEio;
    }

    // Destination parent + leaf (assumes same filesystem; cross-fs rename
    // returns EXDEV on Linux -- a hobby-OS follow-up).
    const char* rel_new = nullptr;
    (void)cinux::fs::vfs_resolve(resolved_newpath, &rel_new);
    cinux::fs::PathBuf new_parent_buf;
    const char*        new_leaf = nullptr;
    uint32_t           new_len  = 0;
    if (!split_pathname(rel_new, new_parent_buf, &new_leaf, &new_len)) {
        return -kEinval;
    }
    auto new_parent_result = fs->lookup(new_parent_buf);
    if (!new_parent_result.ok()) {
        return -to_errno(new_parent_result.error());
    }
    cinux::fs::Inode* new_parent = new_parent_result.value();
    if (new_parent == nullptr) {
        return -kEio;
    }

    auto r = old_parent->ops->rename(old_parent, old_leaf, old_len, new_parent, new_leaf, new_len);
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_rename(uint64_t oldpath_virt, uint64_t newpath_virt, uint64_t, uint64_t, uint64_t,
                   uint64_t) {
    cinux::fs::PathBuf old_resolved;
    cinux::fs::PathBuf new_resolved;
    if (!resolve_user_path(oldpath_virt, old_resolved.data())) {
        return -kEfault;
    }
    if (!resolve_user_path(newpath_virt, new_resolved.data())) {
        return -kEfault;
    }
    return do_rename_kernel(old_resolved.data(), new_resolved.data());
}

}  // namespace cinux::syscall
