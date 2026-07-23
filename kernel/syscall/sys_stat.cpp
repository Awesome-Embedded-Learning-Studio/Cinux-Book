/**
 * @file kernel/syscall/sys_stat.cpp
 * @brief sys_stat and sys_fstat handler implementations (P0a SMAP-layered)
 *
 * Layered, Linux-aligned (see SMAP plan):
 *   - do_stat_kernel / do_fstat_kernel: pure kernel-to-kernel stat logic
 *     (resolved path / fd -> kernel stat). No user memory, may be called by
 *     kernel-internal callers and tests. This is the layer tests target.
 *   - sys_stat / sys_fstat / sys_newfstatat: thin syscall boundaries. They
 *     resolve the user path (still via resolve_user_path today; the path-util
 *     accessor move is a later batch) and copy the result out via copy_to_user.
 *
 * The stat output buffer is the only user pointer owned by the boundary, and it
 * goes through copy_to_user (access_ok + stac). The path side stays on
 * resolve_user_path until the path-family batch retires it.
 */

#include "kernel/syscall/sys_stat.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0a (SMAP): copy_to_user
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/stat.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

// Shared inode->stat() step for do_stat_kernel / do_fstat_kernel.
int64_t do_stat_inode_kernel(cinux::fs::Inode* inode, cinux::fs::stat* kst) {
    if (inode == nullptr || inode->ops == nullptr) {
        return -kEio;
    }
    auto stat_result = inode->ops->stat(inode, kst);
    if (!stat_result.ok()) {
        return -to_errno(stat_result.error());
    }
    return 0;
}

constexpr uint64_t kAtEmptyPath = 0x1000;  ///< AT_EMPTY_PATH: stat dirfd itself
constexpr int64_t  kAtFdcwdStat = -100;    ///< AT_FDCWD

}  // anonymous namespace

// ============================================================
// do_*_kernel: pure kernel-to-kernel stat logic (no user memory)
// ============================================================

int64_t do_stat_kernel(const char* resolved_path, cinux::fs::stat* kst) {
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);
    if (fs == nullptr) {
        kprintf("[SYS_STAT] No filesystem mounted for '%s'\n", resolved_path);
        return -kEnoent;
    }
    auto inode_result = fs->lookup(rel_path);
    if (!inode_result.ok()) {
        kprintf("[SYS_STAT] File not found: '%s'\n", resolved_path);
        return -to_errno(inode_result.error());
    }
    return do_stat_inode_kernel(inode_result.value(), kst);
}

int64_t do_fstat_kernel(int fd, cinux::fs::stat* kst) {
    cinux::fs::File* file = cinux::fs::current_fd_table().get(fd);
    if (file == nullptr || file->inode == nullptr) {
        return -kEbadf;
    }
    return do_stat_inode_kernel(file->inode, kst);
}

// ============================================================
// sys_* boundaries: accessor owns the user stat output buffer
// ============================================================

int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }

    cinux::fs::stat kst;
    int64_t         rc = do_stat_kernel(resolved.data(), &kst);
    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(st_virt), &kst, sizeof(kst))) {
        return -kEfault;
    }
    return 0;
}

int64_t sys_fstat(uint64_t fd, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::stat kst;
    int64_t         rc = do_fstat_kernel(static_cast<int>(fd), &kst);
    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(st_virt), &kst, sizeof(kst))) {
        return -kEfault;
    }
    return 0;
}

// ============================================================
// sys_newfstatat (F10-M1 batch 4) -- musl stat/fstat/lstat entry point
// ============================================================

int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_virt, uint64_t st_virt, uint64_t flags,
                       uint64_t, uint64_t) {
    cinux::fs::stat kst;
    int64_t         rc;

    if (flags & kAtEmptyPath) {
        // stat the dirfd itself (fstat semantics).
        rc = do_fstat_kernel(static_cast<int>(dirfd), &kst);
    } else {
        // Path-based: resolve cwd-relative. A real dirfd would need per-fd path
        // tracking; AT_FDCWD (the only case musl passes) is handled correctly.
        cinux::fs::PathBuf resolved;
        if (!resolve_user_path(path_virt, resolved.data())) {
            return -kEfault;
        }
        (void)dirfd;
        (void)kAtFdcwdStat;
        rc = do_stat_kernel(resolved.data(), &kst);
    }

    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(st_virt), &kst, sizeof(kst))) {
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
