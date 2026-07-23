/**
 * @file kernel/syscall/sys_link.cpp
 * @brief sys_link handler (F-ECO batch 2)
 *
 * Resolves the existing target inode and the new parent directory, then
 * delegates to InodeOps::link() (adds a directory entry + bumps nlink).
 */

#include "kernel/syscall/sys_link.hpp"

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

int64_t do_link_kernel(const char* resolved_oldpath, const char* resolved_newpath) {
    // Resolve the existing target inode.
    const char*            rel_old = nullptr;
    cinux::fs::FileSystem* fs      = cinux::fs::vfs_resolve(resolved_oldpath, &rel_old);
    if (fs == nullptr) {
        kprintf("[SYS_LINK] No filesystem mounted for '%s'\n", resolved_oldpath);
        return -kEnoent;
    }
    auto target_result = fs->lookup(rel_old);
    if (!target_result.ok()) {
        return -to_errno(target_result.error());
    }
    cinux::fs::Inode* target = target_result.value();
    if (target == nullptr) {
        return -kEnoent;
    }

    // Resolve the new parent directory + leaf (assumes same filesystem; cross-fs
    // hard links return EXDEV on Linux -- a hobby-OS follow-up).
    const char* rel_new = nullptr;
    (void)cinux::fs::vfs_resolve(resolved_newpath, &rel_new);

    cinux::fs::PathBuf parent_buf;
    const char*        leaf = nullptr;
    uint32_t           len  = 0;
    if (!split_pathname(rel_new, parent_buf, &leaf, &len)) {
        return -kEinval;
    }
    auto parent_result = fs->lookup(parent_buf);
    if (!parent_result.ok()) {
        return -to_errno(parent_result.error());
    }
    cinux::fs::Inode* parent = parent_result.value();
    if (parent == nullptr || parent->ops == nullptr) {
        return -kEio;
    }

    auto r = parent->ops->link(parent, leaf, len, target);
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_link(uint64_t oldpath_virt, uint64_t newpath_virt, uint64_t, uint64_t, uint64_t,
                 uint64_t) {
    cinux::fs::PathBuf old_resolved;
    cinux::fs::PathBuf new_resolved;
    if (!resolve_user_path(oldpath_virt, old_resolved.data())) {
        return -kEfault;
    }
    if (!resolve_user_path(newpath_virt, new_resolved.data())) {
        return -kEfault;
    }
    return do_link_kernel(old_resolved.data(), new_resolved.data());
}

}  // namespace cinux::syscall
