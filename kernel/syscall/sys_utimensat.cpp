/**
 * @file kernel/syscall/sys_utimensat.cpp
 * @brief sys_utimensat handler (F-ECO batch 2)
 *
 * Reads a user timespec[2], resolves the path to the target inode, and delegates
 * to InodeOps::utimensat(). The "now" case (times==NULL) is a 0 placeholder
 * until a wall-clock source is wired here (follow-up: RTC boot epoch + HPET).
 */

#include "kernel/syscall/sys_utimensat.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_from_user
#include "kernel/errno.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {
using cinux::lib::kprintf;
}

int64_t do_utimensat_kernel(const char* resolved_path, uint64_t atime_sec, uint32_t atime_nsec,
                            uint64_t mtime_sec, uint32_t mtime_nsec) {
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);
    if (fs == nullptr) {
        kprintf("[SYS_UTIMENSAT] No filesystem mounted for '%s'\n", resolved_path);
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

    auto r = inode->ops->utimensat(inode, atime_sec, atime_nsec, mtime_sec, mtime_nsec);
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_utimensat(uint64_t dirfd, uint64_t path_virt, uint64_t times_virt, uint64_t flags,
                      uint64_t, uint64_t) {
    (void)dirfd;   // AT_FDCWD only; per-fd cwd tracking is a follow-up.
    (void)flags;   // AT_SYMLINK_NOFOLLOW / AT_EMPTY_PATH ignored (no symlink-follow yet).

    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }

    // Linux struct timespec { time_t tv_sec; long tv_nsec; } x86_64 = 16B; [2] = 32B.
    struct KernelTimespec {
        int64_t sec;
        int64_t nsec;
    };
    KernelTimespec kts[2] = {{0, 0}, {0, 0}};
    if (times_virt != 0) {
        if (!cinux::user::copy_from_user(kts, reinterpret_cast<const void*>(times_virt),
                                         sizeof(kts))) {
            return -kEfault;
        }
    }
    // times==NULL means "now"; no wall clock is wired here yet, so 0 is a
    // placeholder (follow-up). nsec is accepted but truncated by the ext2 backend.
    uint64_t atime_sec = static_cast<uint64_t>(kts[0].sec);
    uint64_t mtime_sec = static_cast<uint64_t>(kts[1].sec);
    return do_utimensat_kernel(resolved.data(), atime_sec, 0, mtime_sec, 0);
}

}  // namespace cinux::syscall
