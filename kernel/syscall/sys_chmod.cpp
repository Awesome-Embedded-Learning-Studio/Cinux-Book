/**
 * @file kernel/syscall/sys_chmod.cpp
 * @brief sys_chmod handler (F-ECO batch 2)
 *
 * Resolves the path to the target inode and delegates to InodeOps::chmod(); the
 * ext2 backend replaces the low-12 permission bits, keeping the type bits.
 */

#include "kernel/syscall/sys_chmod.hpp"

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

int64_t do_chmod_kernel(const char* resolved_path, uint32_t mode) {
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);
    if (fs == nullptr) {
        kprintf("[SYS_CHMOD] No filesystem mounted for '%s'\n", resolved_path);
        return -kEnoent;
    }

    auto inode_result = fs->lookup(rel_path);
    if (!inode_result.ok()) {
        return -to_errno(inode_result.error());
    }

    cinux::fs::Inode* inode = inode_result.value();  // ref'd by lookup
    if (inode == nullptr || inode->ops == nullptr) {
        cinux::fs::inode_unref(inode);
        return -kEio;
    }

    auto r = inode->ops->chmod(inode, mode);
    cinux::fs::inode_unref(inode);  // drop the lookup ref
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_chmod(uint64_t path_virt, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }
    return do_chmod_kernel(resolved.data(), static_cast<uint32_t>(mode));
}

}  // namespace cinux::syscall
