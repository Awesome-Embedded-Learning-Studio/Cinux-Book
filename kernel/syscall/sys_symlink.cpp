/**
 * @file kernel/syscall/sys_symlink.cpp
 * @brief sys_symlink handler (F-ECO batch 2)
 *
 * Resolves the link path's parent directory and delegates to
 * InodeOps::symlink(); the target string is read verbatim (not canonicalised --
 * a relative target must survive untouched).
 */

#include "kernel/syscall/sys_symlink.hpp"

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
}

int64_t do_symlink_kernel(const char* target, const char* resolved_linkpath) {
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_linkpath, &rel_path);
    if (fs == nullptr) {
        kprintf("[SYS_SYMLINK] No filesystem mounted for '%s'\n", resolved_linkpath);
        return -kEnoent;
    }

    cinux::fs::PathBuf parent_buf;
    const char*        leaf = nullptr;
    uint32_t           len  = 0;
    if (!split_pathname(rel_path, parent_buf, &leaf, &len)) {
        return -kEinval;
    }

    auto parent_result = fs->lookup(parent_buf);
    if (!parent_result.ok()) {
        return -to_errno(parent_result.error());
    }

    cinux::fs::Inode* parent = parent_result.value();  // ref'd by lookup
    if (parent == nullptr || parent->ops == nullptr) {
        cinux::fs::inode_unref(parent);
        return -kEio;
    }

    auto r = parent->ops->symlink(parent, leaf, len, target);
    cinux::fs::inode_unref(parent);  // drop the lookup ref
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_symlink(uint64_t target_virt, uint64_t linkpath_virt, uint64_t, uint64_t, uint64_t,
                    uint64_t) {
    char target[256];
    if (!read_user_path(target_virt, target, sizeof(target))) {
        return -kEfault;
    }

    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(linkpath_virt, resolved.data())) {
        return -kEfault;
    }
    return do_symlink_kernel(target, resolved.data());
}

}  // namespace cinux::syscall
