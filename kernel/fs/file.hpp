/**
 * @file kernel/fs/file.hpp
 * @brief Open-file description and per-process file descriptor table
 *
 * Defines:
 *   - OpenFlags:  flags passed to sys_open (read/write/rdwr)
 *   - File:       an open file with a reference to its Inode, an offset,
 *                 and access flags
 *   - FDTable:    a fixed-size (256-entry) file descriptor table with
 *                 alloc() and close() operations
 *
 * FDTable is intended to be embedded in the Process / Task struct so that
 * each task gets its own descriptor table.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs/inode.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs {

// ============================================================
// Open Flags
// ============================================================

/// Access mode flags for sys_open
enum class OpenFlags : uint32_t {
    RDONLY = 0,
    WRONLY = 1,
    RDWR   = 2,
};

// ============================================================
// File Descriptor Limits
// ============================================================

/// Maximum number of open file descriptors per process
static constexpr uint32_t FD_TABLE_SIZE = 256;

/// Sentinel value indicating an unallocated fd slot
static constexpr int FD_NONE = -1;

// ============================================================
// File Structure (open file description)
// ============================================================

/**
 * @brief Represents an open file within a process
 *
 * A File ties an Inode pointer to an access mode and a seek offset.
 * Multiple File descriptors may share the same Inode but maintain
 * independent offsets.
 */
struct File {
    File(Inode* in, uint64_t off, OpenFlags fl, bool cx = false)
        : inode(in), offset(off), flags(fl), cloexec(cx) {}

    Inode*    inode;    ///< Pointer to the underlying inode (non-null when in use)
    uint64_t  offset;   ///< Current read/write offset in bytes
    OpenFlags flags;    ///< Access mode (RDONLY, WRONLY, RDWR)
    bool      cloexec;  ///< FD_CLOEXEC (F-ECO batch 4: fcntl F_GETFD/F_SETFD)

    mutable cinux::proc::Spinlock offset_lock_;
};

/// Bump an inode's open-description refcount (DEBT-023). Called when a File
/// enters an FDTable (alloc / set / dup / dup2 -- fork/clone-copy go through
/// set()).  Paired with inode_unref, which on the last close invokes
/// InodeOps::release.  File itself stays a plain struct -- it does NOT touch
/// inode on construction or destruction, so test fixtures may free an inode
/// before its File without use-after-free.
void inode_ref(Inode* inode);

/// Drop one open-description refcount; on the last (refcount -> 0) invoke
/// InodeOps::release (pipe end -> EOF/POLLHUP, socket -> FIN).  Called from
/// FDTable::close / dup2-displace OUTSIDE the fd-table lock (release may do
/// driver work).  Null-safe.
void inode_unref(Inode* inode);

// ============================================================
// File Descriptor Table
// ============================================================

/**
 * @brief Per-process file descriptor table
 *
 * Manages a fixed-size array of File pointers.  Descriptor 0 is
 * reserved for stdin, 1 for stdout, and 2 for stderr (allocated
 * externally by the shell / init setup).
 *
 * Lifetime: the FDTable owns the File objects; close() releases them.
 */
class FDTable {
public:
    /**
     * @brief Construct an empty descriptor table
     *
     * All slots initialised to nullptr.  Refcount starts at 1 (F3-M2 batch 3:
     * CLONE_FILES threads share one table via acquire/release).
     */
    FDTable();

    /**
     * @brief Resource-safety backstop: free every still-open File
     *
     * The normal lifecycle is release(): when the refcount drops to 0 every
     * slot is close()'d (File freed, slot set to nullptr) and the table is
     * `delete`d.  This destructor closes the gaps release() does not cover —
     * a stack-allocated FDTable (unit tests) or any path that skips release()
     * — so no File can ever leak.  It is a no-op on the release() path, where
     * every slot is already nullptr by the time this runs.
     */
    ~FDTable();

    /**
     * @name Reference counting (F3-M2 batch 3)
     *
     * CLONE_FILES threads share one FDTable (acquire bumps the refcount); fork
     * and clone-without-FILES each get a private copy.  release() frees the
     * table (and closes every live File) when the last reference drops.
     */
    ///@{
    void acquire();
    void release();
    ///@}

    /** Current reference count (diagnostics / tests). */
    uint32_t refcount() const { return refcount_; }

    /**
     * @brief Allocate a file descriptor and assign a File to it
     *
     * Searches for the first free slot, creates a File entry for the
     * given inode and flags, and returns the descriptor index.
     *
     * @param inode  The inode to associate with the new descriptor
     * @param flags  Access mode for the open file
     * @return Non-negative descriptor index on success, or FD_NONE (-1)
     *         if the table is full
     */
    int alloc(Inode* inode, OpenFlags flags);

    /**
     * @brief Close a file descriptor and release its File entry
     *
     * @param fd  Descriptor index to close
     * @return 0 on success, or -1 if fd is out of range or already closed
     */
    int close(int fd);

    /**
     * @brief Retrieve the File pointer for a given descriptor
     *
     * @param fd  Descriptor index
     * @return Pointer to the File, or nullptr if fd is invalid / unused
     */
    File* get(int fd) const;

    /**
     * @brief Force-set a File at a specific descriptor slot
     *
     * Replaces whatever is currently at @p fd (if anything) with the
     * given File pointer.  The caller is responsible for ensuring that
     * any previous File at this slot has been properly released.
     *
     * This is used by sys_pipe and future fd redirection (dup2) to
     * install a File at a well-known descriptor number (e.g. 0, 1, 2).
     *
     * @param fd    Descriptor index (must be in [0, FD_TABLE_SIZE))
     * @param file  File pointer to install (ownership transferred to FDTable)
     * @return true on success, false if fd is out of range
     */
    bool set(int fd, File* file);

    /**
     * @name fd duplication (F-ECO batch 4: dup / dup2 / fcntl F_DUPFD)
     *
     * Each duplicate is a NEW File (independent open file description), copying
     * the source inode + offset + flags + cloexec.  This is NOT Linux's shared
     * description (two fds sharing one offset) -- it is a hobby-OS simplification
     * that avoids refcounting File, and is sufficient for sh redirect (the new fd
     * reaches the same inode/pipe).  Shared-description semantics is a follow-up.
     */
    ///@{
    /// Copy @p oldfd to the lowest free fd >= @p min_fd.  Returns the new fd,
    /// or FD_NONE if @p oldfd is invalid / the table is full.
    int dup(int oldfd, int min_fd);
    /// Copy @p oldfd to @p newfd (closing @p newfd first if open).  Returns
    /// @p newfd, or FD_NONE if either fd is out of range / @p oldfd is invalid.
    /// If @p oldfd == @p newfd and valid, returns @p newfd (Linux no-op).
    int dup2(int oldfd, int newfd);
    ///@}

private:
    /// Fixed-size array of File pointers (nullptr = unused slot)
    File*                         fds_[FD_TABLE_SIZE];
    mutable cinux::proc::Spinlock lock_;
    uint32_t                      refcount_;  ///< F3-M2 batch 3: shared by CLONE_FILES threads
};

}  // namespace cinux::fs
