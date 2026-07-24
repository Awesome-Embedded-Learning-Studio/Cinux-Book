/**
 * @file kernel/syscall/sys_readlink.cpp
 * @brief sys_readlink handler (F-ECO batch 2)
 *
 * Resolves the path to the symlink inode (lookup does not follow symlinks, so
 * the inode IS the link) and delegates to InodeOps::readlink(); copies the
 * kernel result out to the user buffer. Returns bytes written, no NUL.
 *
 * The link target is staged in a 256-byte stack buffer (hobby-OS cap); targets
 * longer than 255 bytes are truncated. PATH_MAX-sized targets are a follow-up.
 */

#include "kernel/syscall/sys_readlink.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user
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

int64_t do_readlink_kernel(const char* resolved_path, char* buf, uint64_t buf_size) {
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);
    if (fs == nullptr) {
        kprintf("[SYS_READLINK] No filesystem mounted for '%s'\n", resolved_path);
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

    auto r = inode->ops->readlink(inode, buf, buf_size);
    cinux::fs::inode_unref(inode);  // drop the lookup ref
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return r.value();
}

int64_t sys_readlink(uint64_t path_virt, uint64_t buf_virt, uint64_t buf_size, uint64_t, uint64_t,
                     uint64_t) {
    if (buf_size == 0) {
        return -kEinval;
    }

    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }

    char     kbuf[256];
    uint64_t cap = (buf_size < 256) ? buf_size : 256;
    int64_t  n   = do_readlink_kernel(resolved.data(), kbuf, cap);
    if (n < 0) {
        return n;
    }

    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(buf_virt), kbuf,
                                   static_cast<uint64_t>(n))) {
        return -kEfault;
    }
    return n;
}

}  // namespace cinux::syscall
