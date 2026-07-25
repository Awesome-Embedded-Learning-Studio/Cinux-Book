/**
 * @file kernel/fs/tmpfs/tmpfs.hpp
 * @brief TmpFs -- in-memory writable filesystem (F6-M4 / GCC self-host batch 1)
 *
 * TmpFs is a virtual, memory-only filesystem with no on-disk backend.  Unlike
 * DevFS/ProcFS (whose structure is fixed at mount time), TmpFs mutates its own
 * directory tree at runtime: userspace can create / remove files and
 * directories, and regular-file contents live in heap buffers.  It is the
 * backing store for /tmp (mounted at boot in batch 2), where GCC / cc1 / as /
 * ld write intermediate *.o / *.s during a compile.
 *
 * Inode model: each entry is a heap-allocated TmpNode that embeds an Inode
 * whose fs_private points back at the node, so the InodeOps recover the node
 * via a static_cast (the same fs_private=this trick DevFS uses, extended to
 * per-node mutable content).  Directories chain their children in a
 * singly-linked sibling list (unbounded, unlike DevFS's fixed node table);
 * regular files hold a growable byte buffer.  All tree mutations and content
 * I/O are serialised by one per-FS Spinlock -- low contention (/tmp-only),
 * mirrors the single g_mount_lock in vfs_mount.cpp, and avoids any per-node
 * lock that could deadlock on parent/child nesting.
 *
 * tmpfs.cpp is pure logic (no kprintf / kernel-only I/O) so it links cleanly
 * into both the kernel and the host unit tests; boot wiring (constructing a
 * TmpFs and registering it at /tmp) lives separately in tmpfs_init.cpp per the
 * §14 file-gate pattern, added in batch 2.
 *
 * Page-cache routing: TmpFs leaves InodeOps::is_page_cacheable() at its default
 * false, so sys_read routes direct to ops->read and sys_write always calls
 * ops->write -- tmpfs content never enters the disk-backed PageCache.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "fs/vfs_filesystem.hpp"
#include "kernel/proc/sync.hpp"  // Spinlock

namespace cinux::fs {

/// Maximum length of a tmpfs entry name, including the NUL terminator.
/// Matches Linux TMPFS / NAME_MAX (255) so GCC's temp-file names fit.
static constexpr uint32_t kTmpfsNameMax = 255;

/// Linux st_mode file-type bits used by TmpFs.  Regular files are S_IFREG,
/// directories S_IFDIR.
static constexpr uint32_t kTmpfsSIfReg = 0x8000;
static constexpr uint32_t kTmpfsSIfDir = 0x4000;

/// Capacity is grown in 4 KiB increments so a stream of incremental writes (the
/// GCC pattern: many small write() calls into one *.o) does not reallocate per
/// byte.  Mirrors the page granularity the rest of the MM layer uses.
static constexpr uint64_t kTmpfsGrowthAlign = 4096;

struct TmpNode;  // defined in tmpfs.cpp; TmpFs holds it opaquely

/**
 * @brief In-memory writable filesystem (dynamic inodes, heap file content)
 *
 * Usage:
 *   TmpFs tfs;
 *   tfs.mount();
 *   vfs_mount_add("/tmp", &tfs);   // batch 2 boot wiring
 *   Inode* root = tfs.lookup("");  // -> root directory
 *
 * Files and directories are created on demand via the InodeOps (sys_open with
 * O_CREAT, sys_mkdir, ...); nothing is pre-populated at mount beyond the root.
 */
class TmpFs : public FileSystem {
public:
    TmpFs();
    ~TmpFs() override;

    cinux::lib::ErrorOr<void>   mount() override;
    cinux::lib::ErrorOr<Inode*> lookup(const char* path) override;

    /// F-USABILITY batch 1a: single-component lookup for the vfs_lookup layer.
    /// Resolves one name within parent via find_child (the same primitive
    /// lookup() walks with). Implementation in tmpfs.cpp (uses TmpNode).
    cinux::lib::ErrorOr<Inode*> lookup_child(const Inode* parent,
                                             const char* name,
                                             uint32_t namelen) override;

    /// Allocate the next inode number.  Called by the create / mkdir ops.
    uint64_t alloc_ino() { return next_ino_++; }

    /// The shared InodeOps for regular files / directories.  The create / mkdir
    /// ops stamp a fresh TmpNode's inode.ops with one of these (all inodes of a
    /// kind share the single ops instance, like DevFS shares one DevDirOps).
    InodeOps* file_ops() const { return file_ops_; }
    InodeOps* dir_ops() const { return dir_ops_; }

    /// Serialises all tree mutation and content I/O.  Public because the
    /// anonymous-namespace InodeOps in tmpfs.cpp recover the TmpFs via a
    /// TmpNode back-pointer and must take this lock; mirrors File::offset_lock_
    /// being public in file.hpp.
    cinux::proc::Spinlock lock_;

private:
    TmpNode* root_{nullptr};
    uint64_t next_ino_{2};  // 1 is reserved for the root directory

    // One InodeOps instance per inode kind, owned.  Allocated in mount(), freed
    // in ~TmpFs.  All inodes of a kind share the single ops instance, exactly
    // as DevFS shares one DevDirOps across the root.
    InodeOps* file_ops_{nullptr};
    InodeOps* dir_ops_{nullptr};

    bool mounted_{false};
};

/**
 * @brief Boot hook (batch 2): construct a TmpFs, mount it, and register it at
 *        /tmp.  Kernel-only implementation in tmpfs_init.cpp (§14 file gate).
 *
 * @return true on success, false if mount() or vfs_mount_add(/tmp) fails.
 */
namespace tmpfs {
bool init();
}  // namespace tmpfs

}  // namespace cinux::fs
