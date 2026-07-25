/**
 * @file kernel/syscall/sys_mount.cpp
 * @brief sys_mount handler implementation (F6-M1)
 *
 * fstype-driven filesystem factory:
 *   - tmpfs: heap-allocated per mount, owned (sys_umount2 reclaims the tree).
 *   - proc / devfs: the boot singletons, shared (owned=false: a second mount
 *     point never frees the singleton).
 *   - ext2 / ext4: source is a block-device path (e.g. /dev/sda).  Resolved to
 *     an Inode, whose block_device() exposes the IBlockDevice; a fresh Ext2 is
 *     mounted over it (ext4 reuses Ext2 -- extent inodes route through it).
 *   - anything else (ramfs does not exist, fat/xfs/... out of scope): -ENODEV.
 *
 * @p flags (MS_*) are accepted for Linux ABI parity but not yet modelled.  The
 * boot /tmp mount uses the static g_tmpfs via tmpfs::init() (tmpfs_init.cpp).
 */

#include "kernel/syscall/sys_mount.hpp"

#include <stdint.h>

#include <memory>  // std::unique_ptr (RAII over raw new/delete; EXEMPT-reviewed)

#include "kernel/drivers/block_device.hpp"  // IBlockDevice
#include "kernel/errno.hpp"
#include "kernel/fs/devfs/devfs.hpp"        // devfs::instance() (sys_mount -t devfs)
#include "libs/ext2/ext2.hpp"          // Ext2 (sys_mount -t ext2/ext4)
#include "kernel/fs/file.hpp"               // inode_unref
#include "kernel/fs/path.hpp"               // PathBuf
#include "kernel/fs/procfs/procfs.hpp"      // procfs::instance() (sys_mount -t proc)
#include "kernel/fs/tmpfs/tmpfs.hpp"        // TmpFs
#include "kernel/fs/vfs_lookup.hpp"         // vfs_lookup (source -> Inode)
#include "kernel/fs/vfs_mount.hpp"          // vfs_mount_add
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"            // strcmp
#include "kernel/syscall/path_util.hpp"     // resolve_user_path / read_user_path

namespace cinux::syscall {

using cinux::fs::FileSystem;
using cinux::fs::TmpFs;
using cinux::fs::vfs_mount_add;
using cinux::lib::kprintf;

int64_t do_mount_kernel(const char* source, const char* target, const char* fstype, uint64_t flags) {
    // MS_* flags are accepted for Linux ABI parity but not yet modelled.
    if (target == nullptr || fstype == nullptr || target[0] == '\0') {
        return -kEinval;
    }

    // ---- tmpfs: heap-allocated per mount; owned (sys_umount2 frees the tree) ----
    if (strcmp(fstype, "tmpfs") == 0) {
        // unique_ptr owns TmpFs until mount() succeeds; auto-freed on any error leg.
        std::unique_ptr<TmpFs> tfs(new TmpFs());
        auto                   m = tfs->mount();
        if (!m.ok()) {
            kprintf("[SYS_MOUNT] tmpfs mount() failed: %s\n", cinux::lib::error_string(m.error()));
            return -to_errno(m.error());
        }
        FileSystem* fs = tfs.release();
        if (!vfs_mount_add(target, fs, /*owned=*/true)) {
            kprintf("[SYS_MOUNT] mount table full, cannot mount '%s'\n", target);
            delete fs;  // mount table did not take ownership; reclaim
            return -kEnomem;
        }
        return 0;
    }

    // ---- proc / devfs: boot singletons.  Mounting at a second path shares the
    // one instance; owned=false means sys_umount2 detaches the mount point but
    // never frees the singleton (it outlives any one mount point, as at /proc).
    if (strcmp(fstype, "proc") == 0 || strcmp(fstype, "devfs") == 0) {
        FileSystem* fs = (strcmp(fstype, "proc") == 0)
                             ? static_cast<FileSystem*>(cinux::fs::procfs::instance())
                             : static_cast<FileSystem*>(cinux::fs::devfs::instance());
        if (fs == nullptr) {
            return -kEnodev;  // the FS was never initialised at boot
        }
        if (!vfs_mount_add(target, fs, /*owned=*/false)) {
            kprintf("[SYS_MOUNT] mount table full, cannot mount '%s'\n", target);
            return -kEnomem;
        }
        return 0;
    }

    // ---- ext2 / ext4: source is a block-device path (e.g. /dev/sda).  Resolve
    // it to an Inode, pull the IBlockDevice via block_device(), and mount a fresh
    // Ext2 over it.  NoFollow: /dev/sda is the device node itself, not a symlink
    // to follow.  ENXIO (Linux) when source is not a block device.
    if (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ext4") == 0) {
        if (source == nullptr || source[0] == '\0') {
            return -kEinval;
        }
        auto lr = cinux::fs::vfs_lookup(source,
                                        static_cast<uint32_t>(cinux::fs::LookupFlag::NoFollow), "/");
        if (!lr.ok()) {
            return -to_errno(lr.error());
        }
        cinux::fs::Inode*                ino = lr.value().target;
        cinux::drivers::IBlockDevice*    dev =
            (ino != nullptr && ino->ops != nullptr) ? ino->ops->block_device(ino) : nullptr;
        if (ino != nullptr) {
            cinux::fs::inode_unref(ino);  // vfs_lookup returned a ref
        }
        if (dev == nullptr) {
            return -kEnxio;  // source resolves, but is not a block device
        }
        std::unique_ptr<cinux::fs::Ext2> ext2(new cinux::fs::Ext2(dev));
        auto                             m = ext2->mount();
        if (!m.ok()) {
            kprintf("[SYS_MOUNT] ext2 mount() failed: %s\n", cinux::lib::error_string(m.error()));
            return -to_errno(m.error());
        }
        FileSystem* fs = ext2.release();
        if (!vfs_mount_add(target, fs, /*owned=*/true)) {
            kprintf("[SYS_MOUNT] mount table full, cannot mount '%s'\n", target);
            delete fs;
            return -kEnomem;
        }
        return 0;
    }

    // ---- ramfs (no such FS) / fat / xfs / ... : not supported.  Linux returns
    // ENODEV for an unknown filesystem type.
    (void)source;
    (void)flags;
    kprintf("[SYS_MOUNT] unknown filesystem type '%s'\n", fstype);
    return -kEnodev;
}

int64_t sys_mount(uint64_t source_virt, uint64_t target_virt, uint64_t fstype_virt, uint64_t flags,
                  [[maybe_unused]] uint64_t data, uint64_t) {
    // mount options string -- not yet parsed

    cinux::fs::PathBuf target;
    if (!resolve_user_path(target_virt, target.data())) {
        return -kEfault;
    }

    char fstype[32];
    if (!read_user_path(fstype_virt, fstype, sizeof(fstype))) {
        return -kEfault;
    }

    // source: read only when the fstype needs a backing device (ext2/ext4);
    // tmpfs/proc/devfs ignore it (Linux permits a NULL source for source-less FS).
    cinux::fs::PathBuf source;
    const char*        source_ptr = nullptr;
    if (source_virt != 0 && (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ext4") == 0)) {
        if (!resolve_user_path(source_virt, source.data())) {
            return -kEfault;
        }
        source_ptr = source.data();
    }

    return do_mount_kernel(source_ptr, target.data(), fstype, flags);
}

}  // namespace cinux::syscall
