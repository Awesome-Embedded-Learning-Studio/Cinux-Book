/**
 * @file kernel/syscall/sys_stat.cpp
 * @brief sys_stat and sys_fstat handler implementations
 *
 * sys_stat resolves a path through the VFS, calls InodeOps::stat(),
 * and copies the result to the user buffer.
 * sys_fstat looks up an FD in the global FD table and does the same.
 */

#include "kernel/syscall/sys_stat.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/stat.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(st_virt)) {
        return -kEfault;
    }

    // Step 1: Resolve the path (cwd-aware)
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }

    // Step 2: Resolve through the VFS mount table
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_STAT] No filesystem mounted for '%s'\n", resolved.data());
        return -kEnoent;
    }

    // Step 3: Look up the inode
    auto inode_result = fs->lookup(rel_path);
    if (!inode_result.ok()) {
        kprintf("[SYS_STAT] File not found: '%s'\n", resolved.data());
        return -to_errno(inode_result.error());
    }
    cinux::fs::Inode* inode = inode_result.value();

    // Step 4: Call stat()
    if (inode->ops == nullptr) {
        return -kEio;
    }

    cinux::fs::stat kst;
    auto            stat_result = inode->ops->stat(inode, &kst);
    if (!stat_result.ok()) {
        return -to_errno(stat_result.error());
    }

    // Step 5: Copy to user buffer
    auto* user_st = reinterpret_cast<cinux::fs::stat*>(st_virt);
    memcpy(user_st, &kst, sizeof(cinux::fs::stat));

    return 0;
}

int64_t sys_fstat(uint64_t fd, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(st_virt)) {
        return -kEfault;
    }

    // Step 1: Look up the FD
    cinux::fs::File* file = cinux::fs::current_fd_table().get(static_cast<int>(fd));

    if (file == nullptr || file->inode == nullptr) {
        return -kEbadf;
    }

    cinux::fs::Inode* inode = file->inode;

    // Step 2: Call stat()
    if (inode->ops == nullptr) {
        return -kEio;
    }

    cinux::fs::stat kst;
    auto            stat_result = inode->ops->stat(inode, &kst);
    if (!stat_result.ok()) {
        return -to_errno(stat_result.error());
    }

    // Step 3: Copy to user buffer
    auto* user_st = reinterpret_cast<cinux::fs::stat*>(st_virt);
    memcpy(user_st, &kst, sizeof(cinux::fs::stat));

    return 0;
}

// ============================================================
// sys_newfstatat (F10-M1 batch 4) -- musl stat/fstat/lstat entry point
// ============================================================

namespace {

constexpr uint64_t kAtEmptyPath = 0x1000;  ///< operate on dirfd itself (empty path)
constexpr int64_t  kAtFdcwdStat = -100;    ///< AT_FDCWD

/// Fill a user stat buffer from an inode; returns 0 or -errno.
int64_t fill_user_stat(cinux::fs::Inode* inode, uint64_t st_virt) {
    if (inode == nullptr || inode->ops == nullptr) {
        return -kEio;
    }
    cinux::fs::stat kst;
    auto            stat_result = inode->ops->stat(inode, &kst);
    if (!stat_result.ok()) {
        return -to_errno(stat_result.error());
    }
    memcpy(reinterpret_cast<cinux::fs::stat*>(st_virt), &kst, sizeof(cinux::fs::stat));
    return 0;
}

}  // anonymous namespace

int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_virt, uint64_t st_virt, uint64_t flags,
                       uint64_t, uint64_t) {
    if (!validate_user_ptr(st_virt)) {
        return -kEfault;
    }

    // AT_EMPTY_PATH: stat the dirfd itself (fstat semantics).
    if (flags & kAtEmptyPath) {
        cinux::fs::File* file = cinux::fs::current_fd_table().get(static_cast<int>(dirfd));
        if (file == nullptr || file->inode == nullptr) {
            return -kEbadf;
        }
        return fill_user_stat(file->inode, st_virt);
    }

    // Path-based: resolve cwd-relative.  A real dirfd would need per-fd path
    // tracking; AT_FDCWD (the only case musl passes) is handled correctly.
    (void)dirfd;

    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }

    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);
    if (fs == nullptr) {
        return -kEnoent;
    }

    auto inode_result = fs->lookup(rel_path);
    if (!inode_result.ok()) {
        return -to_errno(inode_result.error());
    }
    (void)kAtFdcwdStat;
    return fill_user_stat(inode_result.value(), st_virt);
}

}  // namespace cinux::syscall
