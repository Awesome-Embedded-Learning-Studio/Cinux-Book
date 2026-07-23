/**
 * @file kernel/fs/procfs.cpp
 * @brief ProcFS implementation: directory + pseudo-file InodeOps + ProcFs backend
 *
 * Defines the inode behaviour as InodeOps subclasses (ProcRootDirOps,
 * ProcPidDirOps, ProcStatFileOps, ProcCmdlineFileOps) in an anonymous namespace,
 * then the ProcFs FileSystem backend that owns the per-PID inode pools.
 * procfs.cpp reads the kernel task registry (signal_find_task_by_pid /
 * signal_nth_task_pid) and the live Task fields, and is therefore kernel-linked
 * -- it is NOT part of the host unit tests (DevFS stays host-testable by
 * injecting a CharSink; ProcFS has no such injection seam, so it is exercised
 * via the in-QEMU run_procfs_tests).
 */

#include "procfs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/lib/string.hpp"
#include "kernel/proc/pid.hpp"     // PidAllocator::PID_MAX (static_assert drift guard)
#include "kernel/proc/signal.hpp"  // signal_find_task_by_pid / signal_nth_task_pid
#include "procfs_content.hpp"      // format_proc_stat / format_proc_cmdline

// ProcFs's fixed PID-indexed pools assume the PID bound matches the allocator.
// If PidAllocator::PID_MAX ever changes, this fires at compile time.
static_assert(cinux::fs::kProcPidMax == cinux::proc::PidAllocator::PID_MAX,
              "ProcFs PID pool bound must match PidAllocator::PID_MAX");

namespace cinux::fs {

using cinux::lib::Error;
using cinux::lib::ErrorOr;
using cinux::proc::signal_find_task_by_pid;
using cinux::proc::signal_nth_task_pid;

namespace {

// ============================================================
// Shared helpers
// ============================================================

/// Fill a struct stat for a ProcFS directory inode (root or a /proc/<pid> dir).
/// Directories are read-only (0555); st_ino echoes the inode number (the PID for
/// a per-PID dir, 1 for root).  Zeroed first so no kernel-stack bytes leak.
void fill_dir_stat(const Inode* inode, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_ino     = inode->ino;
    st->st_nlink   = 2;
    st->st_mode    = kProcSIfDir | 0555;
    st->st_blksize = 4096;
}

/// Emit the "." (index 0) or ".." (index 1) readdir entry into @p name.
/// Returns 1 on a match, -1 (via Error) when @p name_max is too small, or leaves
/// the caller to handle index >= 2.  Centralised so every ProcFS directory ops
/// shares one dot/dotdot spelling.
ErrorOr<int64_t> fill_dot_entry(uint64_t index, char* name, uint64_t name_max) {
    if (index == 0) {
        if (name_max < 2) {
            return Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        if (name_max < 3) {
            return Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        return 1;
    }
    return 0;  // not a dot entry; caller handles index >= 2
}

/// Parse a non-empty decimal run at @p s as a PID.  On success returns true and
/// sets *@p pid (1..kProcPidMax) and *@p rest to the first char after the digits.
/// Inputs that would overflow the PID bound (e.g. "99999") are rejected, so the
/// caller never indexes the inode pool out of range.
bool parse_pid(const char* s, int* pid, const char** rest) {
    if (s == nullptr || s[0] < '0' || s[0] > '9') {
        return false;
    }
    int         v = 0;
    const char* p = s;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
        if (v > kProcPidMax) {
            return false;  // out of the allocator's range
        }
    }
    *pid  = v;
    *rest = p;
    return true;
}

// ============================================================
// Pseudo-file stat/copy helpers (content itself is in procfs_content.cpp)
// ============================================================

/// Fill a struct stat for a ProcFS regular pseudo-file (stat / cmdline).
/// Read-only (0444); st_ino echoes the PID.  Zeroed first -- no leak.
void fill_file_stat(const Inode* inode, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_ino     = inode->ino;
    st->st_nlink   = 1;
    st->st_mode    = kProcSIfReg | 0444;
    st->st_blksize = 4096;
}

/// Copy min(count, len-offset) bytes of generated pseudo-file content into the
/// caller's buffer.  Returns the byte count (0 once past the end = EOF).  Lets
/// the read ops share one offset/EOF handling path.
ErrorOr<int64_t> copy_pseudo(const char* content, uint32_t len, uint64_t offset, void* buf,
                             uint64_t count) {
    if (offset >= len) {
        return 0;  // EOF
    }
    uint64_t avail = static_cast<uint64_t>(len) - offset;
    uint64_t n     = count < avail ? count : avail;
    memcpy(buf, content + offset, n);
    return static_cast<int64_t>(n);
}

// ============================================================
// /proc root directory -- readdir snapshots the live PID registry (batch 2)
// ============================================================

class ProcRootDirOps : public InodeOps {
public:
    ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name,
                             uint64_t name_max) override;
    ErrorOr<void>    stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        fill_dir_stat(inode, st);
        return {};
    }
};

// ============================================================
// /proc/<pid> directory -- readdir lists the per-process files (batch 3)
// ============================================================

class ProcPidDirOps : public InodeOps {
public:
    ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name,
                             uint64_t name_max) override;
    ErrorOr<void>    stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        fill_dir_stat(inode, st);
        return {};
    }
};

// The root directory lists one entry per live PID.  Indices 0/1 are "." / "..";
// index i>=2 maps to the (i-2)-th PID in a registry snapshot taken on each call
// (Linux /proc readdir recomputes the task set per getdents the same way).  The
// registry is normally stable across one listing, so successive indices see a
// consistent snapshot.
ErrorOr<int64_t> ProcRootDirOps::readdir(const Inode* inode, uint64_t index, char* name,
                                         uint64_t name_max) {
    if (inode == nullptr || name == nullptr || name_max == 0) {
        return Error::InvalidArgument;
    }

    auto dot = fill_dot_entry(index, name, name_max);
    if (!dot.ok() || dot.value() == 1) {
        return dot;  // "." / ".." handled, or a name_max too small to spell them
    }

    // index >= 2: the (index-2)-th live PID.  Walking the registry to the index
    // directly (rather than snapshotting it onto the stack) keeps this frame
    // under the kernel's 1024-byte limit.  utoa's contract is 11 bytes; readdir
    // name buffers are PROCFS_NAME_MAX (32), so the guard never trips in
    // practice but honours the contract for any caller.
    if (name_max < 11) {
        return Error::InvalidArgument;
    }
    int pid;
    if (!signal_nth_task_pid(static_cast<uint32_t>(index - 2), &pid)) {
        return 0;  // past the last live PID
    }
    utoa(name, static_cast<uint32_t>(pid));
    return 1;
}

// ============================================================
// /proc/<pid>/stat and /proc/<pid>/cmdline -- read-only pseudo-files
// ============================================================

class ProcStatFileOps : public InodeOps {
public:
    ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override;
    ErrorOr<void>    stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        fill_file_stat(inode, st);
        return {};
    }
};

class ProcCmdlineFileOps : public InodeOps {
public:
    ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override;
    ErrorOr<void>    stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        fill_file_stat(inode, st);
        return {};
    }
};

ErrorOr<int64_t> ProcPidDirOps::readdir(const Inode* inode, uint64_t index, char* name,
                                        uint64_t name_max) {
    if (inode == nullptr || name == nullptr || name_max == 0) {
        return Error::InvalidArgument;
    }
    auto dot = fill_dot_entry(index, name, name_max);
    if (!dot.ok() || dot.value() == 1) {
        return dot;
    }
    // index 2 -> "stat", index 3 -> "cmdline"; anything beyond is end-of-dir.
    switch (index) {
    case 2:
        if (name_max < 5) {
            return Error::InvalidArgument;  // "stat" + NUL
        }
        memcpy(name, "stat", 5);
        return 1;
    case 3:
        if (name_max < 8) {
            return Error::InvalidArgument;  // "cmdline" + NUL
        }
        memcpy(name, "cmdline", 8);
        return 1;
    default:
        return 0;
    }
}

// Generate the stat line fresh on each read from the live Task.  A task that
// exits between lookup and read yields NotFound (the inode existed, the task no
// longer does) -- the documented registry TOCTOU window.
ErrorOr<int64_t> ProcStatFileOps::read(const Inode* inode, uint64_t offset, void* buf,
                                       uint64_t count) {
    if (inode == nullptr || buf == nullptr) {
        return Error::InvalidArgument;
    }
    int  pid = static_cast<int>(inode->ino);
    auto t   = signal_find_task_by_pid(pid);
    if (t == nullptr) {
        return Error::NotFound;
    }
    char     line[kProcStatLineMax];
    uint32_t len = format_proc_stat(t, line, sizeof(line));
    return copy_pseudo(line, len, offset, buf, count);
}

ErrorOr<int64_t> ProcCmdlineFileOps::read(const Inode* inode, uint64_t offset, void* buf,
                                          uint64_t count) {
    if (inode == nullptr || buf == nullptr) {
        return Error::InvalidArgument;
    }
    int  pid = static_cast<int>(inode->ino);
    auto t   = signal_find_task_by_pid(pid);
    if (t == nullptr) {
        return Error::NotFound;
    }
    char     content[kProcCmdlineMax];
    uint32_t len = format_proc_cmdline(t, content, sizeof(content));
    return copy_pseudo(content, len, offset, buf, count);
}

}  // anonymous namespace

// ============================================================
// ProcFs
// ============================================================

ProcFs::ProcFs() = default;

ProcFs::~ProcFs() {
    delete root_dir_ops_;
    delete pid_dir_ops_;
    delete stat_file_ops_;
    delete cmdline_file_ops_;
}

ErrorOr<void> ProcFs::mount() {
    // Idempotent: a second mount() is a no-op (mirrors DevFs).
    if (mounted_) {
        return {};
    }

    root_dir_ops_     = new ProcRootDirOps();
    pid_dir_ops_      = new ProcPidDirOps();
    stat_file_ops_    = new ProcStatFileOps();
    cmdline_file_ops_ = new ProcCmdlineFileOps();

    // Root directory inode: readdir snapshots the live PID registry.
    root_inode_.ino        = 1;
    root_inode_.type       = InodeType::Directory;
    root_inode_.ops        = root_dir_ops_;
    root_inode_.fs_private = this;
    root_inode_.mode       = kProcSIfDir | 0555;
    root_inode_.nlink      = 2;

    // Per-PID inodes, stamped eagerly.  Existing as an inode does not imply the
    // PID is live -- lookup() gates every hand-out on signal_find_task_by_pid,
    // so /proc only ever exposes live tasks.
    for (int pid = 1; pid <= kProcPidMax; ++pid) {
        Inode& d     = pid_dir_inodes_[pid];
        d.ino        = static_cast<uint64_t>(pid);
        d.type       = InodeType::Directory;
        d.ops        = pid_dir_ops_;
        d.fs_private = this;
        d.mode       = kProcSIfDir | 0555;
        d.nlink      = 2;

        Inode& s     = stat_inodes_[pid];
        s.ino        = static_cast<uint64_t>(pid);
        s.type       = InodeType::Regular;
        s.ops        = stat_file_ops_;
        s.fs_private = this;
        s.mode       = kProcSIfReg | 0444;
        s.nlink      = 1;

        Inode& c     = cmdline_inodes_[pid];
        c.ino        = static_cast<uint64_t>(pid);
        c.type       = InodeType::Regular;
        c.ops        = cmdline_file_ops_;
        c.fs_private = this;
        c.mode       = kProcSIfReg | 0444;
        c.nlink      = 1;
    }

    mounted_ = true;
    return {};
}

ErrorOr<Inode*> ProcFs::lookup(const char* path) {
    if (path == nullptr) {
        return Error::InvalidArgument;
    }

    // Root directory: empty path or "/".
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return &root_inode_;
    }
    // Strip a single leading '/': vfs_resolve may pass "/1/stat" (mount prefix
    // without trailing slash) or "1/stat" (with trailing slash).
    const char* p = path;
    if (p[0] == '/') {
        ++p;
    }
    if (p[0] == '\0') {
        return &root_inode_;  // trailing slash on the mount root
    }

    int         pid;
    const char* rest;
    if (!parse_pid(p, &pid, &rest)) {
        return Error::NotFound;
    }
    if (pid < 1 || pid > kProcPidMax) {
        return Error::NotFound;
    }

    // "<pid>" -- the per-process directory.  The PID must be live.
    if (rest[0] == '\0') {
        if (signal_find_task_by_pid(pid) == nullptr) {
            return Error::NotFound;  // no such live task
        }
        return &pid_dir_inodes_[pid];
    }

    // "<pid>/<leaf>" -- the per-process pseudo-files.  Re-check liveness so a
    // file under a dead PID is NotFound (the dir may exist as an inode, the task
    // no longer does).
    if (rest[0] != '/') {
        return Error::NotFound;  // "<pid>junk" -- not a path separator
    }
    const char* leaf = rest + 1;
    if (signal_find_task_by_pid(pid) == nullptr) {
        return Error::NotFound;
    }
    if (strcmp(leaf, "stat") == 0) {
        return &stat_inodes_[pid];
    }
    if (strcmp(leaf, "cmdline") == 0) {
        return &cmdline_inodes_[pid];
    }
    return Error::NotFound;
}

}  // namespace cinux::fs
