/**
 * @file kernel/syscall/sys_chown.cpp
 * @brief sys_chown handler (F-ECO batch 2)
 *
 * Resolves the path to the target inode and delegates to InodeOps::chown().
 * uid/gid == 0xFFFFFFFF mean "leave unchanged" (Linux chown(2)).
 */

#include "kernel/syscall/sys_chown.hpp"

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

int64_t do_chown_kernel(const char* resolved_path, uint32_t uid, uint32_t gid) {
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);
    if (fs == nullptr) {
        kprintf("[SYS_CHOWN] No filesystem mounted for '%s'\n", resolved_path);
        return -kEnoent;
    }

    auto inode_result = fs->lookup(rel_path);
    if (!inode_result.ok()) {
        return -to_errno(inode_result.error());
    }

    cinux::fs::Inode* inode = inode_result.value();
    if (inode == nullptr || inode->ops == nullptr) {
        return -kEio;
    }

    auto r = inode->ops->chown(inode, uid, gid);
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_chown(uint64_t path_virt, uint64_t uid, uint64_t gid, uint64_t, uint64_t, uint64_t) {
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }
    return do_chown_kernel(resolved.data(), static_cast<uint32_t>(uid), static_cast<uint32_t>(gid));
}

}  // namespace cinux::syscall
