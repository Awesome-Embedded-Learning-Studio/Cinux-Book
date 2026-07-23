/**
 * @file kernel/fs/procfs.hpp
 * @brief ProcFS -- in-memory /proc filesystem for process introspection (F6-M2)
 *
 * ProcFS is a virtual, memory-only filesystem (no on-disk backend) mounted at
 * /proc.  It exposes the live task set as a directory tree, mirroring Linux's
 * /proc process-introspection model:
 *
 *   /proc                     directory; readdir lists one entry per live PID
 *   /proc/<pid>               directory; readdir lists the per-process files
 *   /proc/<pid>/stat          pseudo-file; content generated on read
 *   /proc/<pid>/cmdline       pseudo-file; content generated on read
 *
 * The root directory is *dynamic* (its entries are the live PIDs snapshotted
 * from the signal task registry at readdir time), which is the key difference
 * from DevFS (F6-M3), whose root is a fixed node table.  The readdir snapshot
 * is taken via signal_enumerate_task_pids (F6-M2 batch 2) under the registry
 * lock; lookup validates a PID via signal_find_task_by_pid, so /proc only ever
 * exposes live tasks (a PID that exits between readdir and lookup yields
 * NotFound, exactly like Linux).
 *
 * Inode identity: close() frees only the File, never the Inode (file.cpp), so
 * every inode a lookup returns must be owned by and outlive ProcFs.  ProcFs
 * holds fixed pools indexed by PID -- root + per-PID directory/stat/cmdline --
 * with each leaf encoding its PID in `ino` and ProcFs in `fs_private`, so the
 * InodeOps subclasses (which receive only the Inode*) can recover both.  One
 * PID maps to one stable inode, so concurrent lookups of different PIDs never
 * race on a shared scratch slot.  PID_MAX (256) bounds the pools.
 *
 * procfs.cpp reads the kernel task registry and is therefore kernel-linked
 * (not host-testable); it is exercised via the in-QEMU test run_procfs_tests.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "fs/vfs_filesystem.hpp"

namespace cinux::fs {

/// Upper bound on PID values.  Must match PidAllocator::PID_MAX
/// (kernel/proc/pid.hpp); ProcFS keeps a fixed inode pool indexed by PID, so
/// the bound is asserted in procfs.cpp.  PIDs are 1..kProcPidMax (slot 0 is
/// unused, matching PidAllocator where PID 0 means "uninitialised").
static constexpr int kProcPidMax = 256;

/// Maximum length of a /proc entry name (decimal PID, or "stat"/"cmdline"),
/// including the NUL terminator.
static constexpr uint32_t PROCFS_NAME_MAX = 32;

/// Linux st_mode file-type bits used by ProcFS entries.  Directories are
/// read-only (0555); pseudo-files are read-only regular files (0444).
static constexpr uint32_t kProcSIfDir = 0x4000;  ///< S_IFDIR
static constexpr uint32_t kProcSIfReg = 0x8000;  ///< S_IFREG

/**
 * @brief In-memory /proc filesystem (process introspection, no on-disk backend)
 *
 * Usage:
 *   ProcFs procfs;
 *   procfs.mount();
 *   vfs_mount_add("/proc", &procfs);
 *   Inode* root = procfs.lookup("");        // -> /proc directory
 *   Inode* dir  = procfs.lookup("1");       // -> /proc/1 directory (live PID 1)
 *   Inode* st   = procfs.lookup("1/stat");  // -> /proc/1/stat pseudo-file
 */
class ProcFs : public FileSystem {
public:
    ProcFs();
    ~ProcFs() override;

    cinux::lib::ErrorOr<void>   mount() override;
    cinux::lib::ErrorOr<Inode*> lookup(const char* path) override;

private:
    // One InodeOps instance per inode kind, owned.  Allocated in mount(), freed
    // in ~ProcFs.  All inodes of a kind share the single ops instance, exactly
    // as DevFs shares one DevDirOps across the root.
    InodeOps* root_dir_ops_{nullptr};
    InodeOps* pid_dir_ops_{nullptr};
    InodeOps* stat_file_ops_{nullptr};
    InodeOps* cmdline_file_ops_{nullptr};

    /// /proc root directory inode.  readdir snapshots the live PID registry.
    Inode root_inode_{};

    /// /proc/<pid> directory inodes, indexed [pid] (slot 0 unused).  ino = pid,
    /// fs_private = this ProcFs.  Stamped eagerly in mount(); lookup still
    /// validates liveness via signal_find_task_by_pid before handing one out.
    Inode pid_dir_inodes_[kProcPidMax + 1]{};

    /// /proc/<pid>/stat pseudo-file inodes, indexed [pid].  ino = pid; read
    /// regenerates the line from the live Task fields.
    Inode stat_inodes_[kProcPidMax + 1]{};

    /// /proc/<pid>/cmdline pseudo-file inodes, indexed [pid].  ino = pid.
    Inode cmdline_inodes_[kProcPidMax + 1]{};

    bool mounted_{false};
};

/**
 * @brief Boot hook: construct ProcFs, mount it, and register it at /proc.
 *
 * Kernel-only implementation in procfs_init.cpp (linked into the kernel, not
 * the host tests).  Separate from procfs.cpp so procfs.cpp stays free of boot
 * I/O (kprintf) -- the §14 file-gate pattern, same as DevFs.
 *
 * @return true on success, false if mount() or vfs_mount_add(/proc) fails.
 */
namespace procfs {
bool init();
}  // namespace procfs

}  // namespace cinux::fs
