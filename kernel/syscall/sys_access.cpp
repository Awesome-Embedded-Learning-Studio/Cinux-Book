/**
 * @file kernel/syscall/sys_access.cpp
 * @brief sys_access handler implementation (F6 / GCC self-host batch 3a)
 *
 * Standard Unix discretionary access check (no ACL / capabilities beyond the
 * root bypass).  Resolves the path, stats the inode, and checks the caller's
 * credentials (current task uid/gid) against the permission bits.  busybox/gcc
 * run as root, so R/W generally succeed; X requires an execute bit -- which is
 * the one denial a root caller can still hit (access(X_OK) on a 0644 file).
 *
 * The access_granted() helper is the reusable permission core; F6's
 * check_permission and a future login() can build on the same rule.
 */

#include "kernel/syscall/sys_access.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"  // inode_unref
#include "kernel/fs/stat.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/proc/process.hpp"    // Task::uid/gid
#include "kernel/proc/scheduler.hpp"  // Scheduler::current()
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

/// Linux access(2) mode bits.
constexpr uint32_t kFOk            = 0;
constexpr uint32_t kROk            = 4;
constexpr uint32_t kWOk            = 2;
constexpr uint32_t kXOk            = 1;
constexpr uint32_t kAccessModeMask = kROk | kWOk | kXOk;  // = 7 (F_OK is 0)

/// Standard Unix permission check (no ACL).  @p imode is the inode's low
/// permission bits (st_mode & 0777), @p iuid/@p igid its owner; @p uid/@p gid
/// the caller; @p want the OR of R_OK/W_OK/X_OK (or F_OK).
bool access_granted(uint32_t imode, uint32_t iuid, uint32_t igid, uint32_t uid, uint32_t gid,
                    uint32_t want) {
    if (want == kFOk) {
        return true;  // existence only -- caller already resolved the path
    }
    if (uid == 0) {
        // root: R/W always permitted; X only if any execute bit is set (mirrors
        // Linux generic_permission's root bypass for the executable facet).
        if ((want & kXOk) == 0) {
            return true;
        }
        return (imode & 0111) != 0;
    }
    // Pick the owner / group / other triple, then require every requested facet.
    uint32_t triple = (uid == iuid)   ? ((imode >> 6) & 7)
                      : (gid == igid) ? ((imode >> 3) & 7)
                                      : (imode & 7);
    return (triple & want) == want;
}

}  // namespace

int64_t do_access_kernel(const char* resolved_path, uint32_t mode) {
    if ((mode & ~kAccessModeMask) != 0) {
        return -kEinval;  // bogus mode bits
    }

    const char*            rel = nullptr;
    cinux::fs::FileSystem* fs  = cinux::fs::vfs_resolve(resolved_path, &rel);
    if (fs == nullptr) {
        return -kEnoent;
    }
    auto inode_r = fs->lookup(rel);
    if (!inode_r.ok()) {
        return -to_errno(inode_r.error());
    }
    cinux::fs::Inode* inode = inode_r.value();  // ref'd by lookup
    if (inode->ops == nullptr) {
        cinux::fs::inode_unref(inode);
        return -kEio;
    }
    if (mode == kFOk) {
        cinux::fs::inode_unref(inode);
        return 0;  // path resolved -> exists
    }

    cinux::fs::stat st;
    auto            s = inode->ops->stat(inode, &st);
    cinux::fs::inode_unref(inode);  // drop the lookup ref (only the stat snapshot is needed below)
    if (!s.ok()) {
        return -to_errno(s.error());
    }

    uint32_t uid  = 0;
    uint32_t gid  = 0;
    auto*    task = cinux::proc::Scheduler::current();
    if (task != nullptr) {
        uid = task->uid;
        gid = task->gid;
    }

    if (access_granted(st.st_mode & 0777, st.st_uid, st.st_gid, uid, gid, mode)) {
        return 0;
    }
    return -kEacces;
}

int64_t sys_access(uint64_t path_virt, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }
    return do_access_kernel(resolved.data(), static_cast<uint32_t>(mode));
}

}  // namespace cinux::syscall
