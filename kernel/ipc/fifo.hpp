/**
 * @file kernel/ipc/fifo.hpp
 * @brief Named FIFO (mkfifo / named pipe) -- F8-M2
 *
 * A named FIFO gives an anonymous Pipe a filesystem name.  mkfifo() registers
 * a name in the in-memory FifoRegistry; opening that name clones a per-open
 * inode bound to the read or write end of a shared Pipe (InodeOps::open, the
 * same cloning hook /dev/ptmx uses).
 *
 * Semantics implemented this milestone (mechanism + kernel-internal + smoke):
 *   - mkfifo(name) -> FifoRegistry::create(name) (the pipe is created lazily on
 *     first open, not at mkfifo time).
 *   - open(name, flags): first open creates the Pipe; the open direction
 *     (O_RDONLY vs O_WRONLY) selects PipeReadOps vs PipeWriteOps; O_NONBLOCK
 *     propagates to the pipe end.  read()/write() then block at the data level
 *     (the pipe's wait queue) until the peer enqueues/drains.
 *
 * Deferred (documented, Linux-faithful behaviour for a real shell):
 *   - open()-level blocking until both ends are present (today a writer can
 *     buffer up to 4 KB before a reader opens; the data-level block still keeps
 *     it correct).
 *   - Destroying the pipe for a fresh "open epoch" after all fds close -- there
 *     is no InodeOps::release hook yet, so a re-open reuses the existing pipe.
 *
 * Per-FIFO state lives in the Fifo pointed to by inode->fs_private, so a single
 * shared FifoOps instance serves every FIFO inode (no InodeType enum change).
 *
 * Namespace: cinux::ipc
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "fs/stat.hpp"  // struct stat
#include "kernel/fs/inode.hpp"
#include "kernel/proc/sync.hpp"  // Spinlock

namespace cinux::ipc {

class Pipe;

// ============================================================
// Constants
// ============================================================

/// Linux st_mode file-type bit for a FIFO (S_IFIFO = 010000 octal).
static constexpr uint32_t kSIfFifo = 0x1000;

/// Maximum FIFO name length (NUL-terminated), matching DevFS DEVFS_NAME_MAX.
static constexpr uint32_t FIFO_NAME_MAX = 32;

/// Maximum number of concurrently registered FIFOs.
static constexpr uint32_t FIFO_REGISTRY_MAX = 16;

/// Inode-number base for FIFO inodes (kept clear of DevFS's small 1-based node
/// inos and PTY's kPtyInoBase range so stat() inos are distinguishable).
static constexpr uint64_t kFifoInoBase = 0xF1F00000ULL;

/// Linux x86_64 open() flag bits the FIFO cloning open() consults.
static constexpr uint64_t kOAccessMask = 0x3;  ///< access mode: 0=RDONLY,1=WRONLY,2=RDWR
static constexpr uint64_t kOWronly     = 0x1;
static constexpr uint64_t kONonblock   = 0x800;  ///< O_NONBLOCK (x86_64)

// ============================================================
// Fifo -- one named pipe
// ============================================================

/**
 * @brief A named FIFO entry: owns the underlying Pipe (lazily created)
 *
 * The Pipe is created on the first open() of this name and shared by every
 * subsequent open.  Lifetime is bounded by the FifoRegistry entry.
 */
struct Fifo {
    Pipe*                 pipe{nullptr};  ///< created on first open; null until then
    cinux::proc::Spinlock lock;           ///< guards lazy pipe creation across racing opens

    /// Return the shared pipe, creating it on the first call.
    Pipe* get_or_create_pipe();
};

// ============================================================
// FifoRegistry -- in-memory name -> FIFO map
// ============================================================

/**
 * @brief Fixed-size in-memory registry of named FIFOs
 *
 * FIFOs are addressed by leaf name (mkfifo registers, DevFS lookup consults,
 * unlink removes).  A fixed table keeps the kernel off <map>/<string>.
 */
class FifoRegistry {
public:
    /// Process-wide singleton (the kernel has one FIFO namespace).
    static FifoRegistry& instance();

    /// Register a new FIFO name.  AlreadyExists if taken, OutOfMemory if full.
    cinux::lib::ErrorOr<void>              create(const char* name);
    /// Look up a registered FIFO by name (no creation).  NotFound if absent.
    cinux::lib::ErrorOr<Fifo*>             lookup(const char* name);
    /// Look up the stable FIFO inode for DevFS dynamic resolution.  The inode's
    /// ops is the shared FifoOps and fs_private points at the entry's Fifo.
    /// NotFound if the name is absent.
    cinux::lib::ErrorOr<cinux::fs::Inode*> lookup_inode(const char* name);
    /// Unregister a name and free its pipe.  No-op if absent.
    void                                   remove(const char* name);

private:
    FifoRegistry() = default;

    struct Entry {
        char             name[FIFO_NAME_MAX];
        bool             used{false};
        Fifo             fifo;
        cinux::fs::Inode inode;  // stable FIFO inode for DevFS lookup (ops=FifoOps)
    };

    Entry                 entries_[FIFO_REGISTRY_MAX];
    cinux::proc::Spinlock lock_;

    /// Index of @p name, or -1.  Caller holds lock_.
    int find_locked(const char* name) const;
};

// ============================================================
// FifoOps -- cloning InodeOps for a FIFO inode
// ============================================================

/**
 * @brief InodeOps for a FIFO inode
 *
 * open() is a cloning open: on first open it creates the pipe held by the Fifo
 * in @p inode->fs_private, then returns a fresh per-open inode bound to the read
 * or write end (per the open direction in @p flags).  The FIFO inode itself is
 * never read/written directly -- the cloned per-open inode carries
 * PipeReadOps/PipeWriteOps.
 */
class FifoOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<cinux::fs::Inode*> open(cinux::fs::Inode* inode, uint64_t flags) override;
    cinux::lib::ErrorOr<void> stat(const cinux::fs::Inode* inode, cinux::fs::stat* st) override;
};

/// Shared singleton ops instance for every FIFO inode.
FifoOps& fifo_ops();

}  // namespace cinux::ipc
