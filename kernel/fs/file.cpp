/**
 * @file kernel/fs/file.cpp
 * @brief Implementation of FDTable -- per-process file descriptor table
 *
 * Provides alloc() / close() / get() operations on a fixed-size (256-entry)
 * array of File pointers.
 *
 * Namespace: cinux::fs
 */

#include "kernel/fs/file.hpp"

#include <utility>  // std::move (FileRef move out of fds_[] under lock)

namespace cinux::fs {

// ============================================================
// Inode refcount helpers (DEBT-023: last-close release)
// ============================================================

void inode_ref(Inode* inode) {
    if (inode != nullptr) {
        __atomic_add_fetch(&inode->refcount, 1, __ATOMIC_ACQ_REL);
    }
}

void inode_unref(Inode* inode) {
    // Drop one open-description reference; on the last (refcount -> 0) invoke
    // InodeOps::release so the fd type can propagate the close (pipe end ->
    // EOF/POLLHUP, socket -> FIN).  Atomic: File objects in different FDTables
    // (post-fork) on different CPUs share one inode.  File itself never touches
    // inode on destruction, so callers/tests may free an inode before its File.
    if (inode != nullptr && inode->ops != nullptr &&
        __atomic_sub_fetch(&inode->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        inode->ops->release(inode);
    }
}

// ============================================================
// Construction
// ============================================================

FDTable::FDTable() : refcount_(1) {
    // fds_[] are FileRef (default-constructed = empty); no manual init needed.
}

// ============================================================
// Destruction (resource-safety backstop)
// ============================================================

// ~FDTable is = default in the header: the FileRef value members unref every
// slot automatically. (Was a manual `delete fds_[i]` loop; FileRef made it
// redundant, and a raw delete would no longer compile against FileRef.)

// ============================================================
// Reference counting (F3-M2 batch 3)
// ============================================================

void FDTable::acquire() {
    // F4-M5 R3 / DEBT-010: atomic refcount (aligned with SharedCwd/SharedSigActions).
    // CLONE_FILES threads on different CPUs share one FDTable (F3-M2), so
    // acquire/release race once APs really run threads.  ACQ_REL pairs the
    // release-to-0 (must see all prior writes) with acquire.
    __atomic_add_fetch(&refcount_, 1, __ATOMIC_ACQ_REL);
}

void FDTable::release() {
    // F4-M5 R3 / DEBT-010: atomic refcount.  Dropped the racy `refcount_ > 0`
    // guard (correct liveness never underflows); the release that brings it to 0
    // owns the cleanup.  At refcount==0 no other reference exists, so reading
    // fds_[] here is race-free; close() takes lock_ itself, so no lock held here.
    if (__atomic_sub_fetch(&refcount_, 1, __ATOMIC_ACQ_REL) == 0) {
        // Last reference: close every live descriptor, then free the table itself.
        for (uint32_t i = 0; i < FD_TABLE_SIZE; ++i) {
            if (fds_[i]) {  // non-empty FileRef
                close(static_cast<int>(i));
            }
        }
        delete this;
    }
}

// ============================================================
// Alloc
// ============================================================

int FDTable::alloc(Inode* inode, OpenFlags flags) {
    auto g = lock_.guard();

    for (uint32_t i = 0; i < FD_TABLE_SIZE; ++i) {
        if (!fds_[i]) {  // empty FileRef = unused slot
            fds_[i] = FileRef(new File(inode, 0, flags));
            inode_ref(inode);  // DEBT-023: this fd holds one inode reference
            return static_cast<int>(i);
        }
    }
    return FD_NONE;
}

// ============================================================
// Close
// ============================================================

int FDTable::close(int fd) {
    FileRef detached;
    Inode*  inode = nullptr;
    {
        auto g = lock_.guard();

        if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
            return -1;
        }
        if (!fds_[fd]) {
            return -1;
        }
        // Detach under the lock; drop OUTSIDE so InodeOps::release (a socket
        // FIN, a pipe close_writer waking blocked peers) cannot block the fd
        // table. inode_unref does the refcount + last-close release (DEBT-023);
        // ~detached then unref's the File (deletes on 0) on scope exit.
        inode    = fds_[fd]->inode;
        detached = std::move(fds_[fd]);  // fds_[fd] becomes empty
    }
    inode_unref(inode);  // --refcount; on last close -> InodeOps::release
    return 0;
    // ~detached (outside lock): FileRef unref -> delete File on 0
}

// ============================================================
// Get
// ============================================================

File* FDTable::get(int fd) const {
    auto g = lock_.guard();

    if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
        return nullptr;
    }
    return fds_[fd].get();
}

// ============================================================
// Set
// ============================================================

bool FDTable::set(int fd, File* file) {
    FileRef displaced;
    {
        auto g = lock_.guard();

        if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
            return false;
        }
        // The incoming File gains an inode reference (DEBT-023), paired with
        // close()'s unref. The previous occupant (if any) is dropped OUTSIDE
        // the lock by ~displaced, so a last-close release cannot block the fd
        // table. (FDTable now owns every installed File; the old "caller owns
        // previous occupant" contract is gone -- RAII manages lifetime.)
        displaced = std::move(fds_[fd]);  // old out (fds_[fd] empty)
        fds_[fd]  = FileRef(file);        // new in (adopt: FileRef +1)
        if (file != nullptr) {
            inode_ref(file->inode);
        }
    }
    return true;
    // ~displaced (outside lock): FileRef unref the previous occupant
}

// ============================================================
// Dup / Dup2 (F-ECO batch 4)
// ============================================================

int FDTable::dup(int oldfd, int min_fd) {
    auto g = lock_.guard();

    if (oldfd < 0 || oldfd >= static_cast<int>(FD_TABLE_SIZE) || !fds_[oldfd]) {
        return FD_NONE;
    }
    int start = min_fd < 0 ? 0 : min_fd;
    for (int i = start; i < static_cast<int>(FD_TABLE_SIZE); ++i) {
        if (!fds_[i]) {
            // Batch 1: still an INDEPENDENT copy (each fd its own offset).
            // Batch 2 changes this to SHARE one File (Linux open-file-description).
            File* src = fds_[oldfd].get();
            fds_[i]   = FileRef(new File(src->inode, src->offset, src->flags, src->cloexec));
            inode_ref(src->inode);  // DEBT-023: the duplicate holds its own ref
            return i;
        }
    }
    return FD_NONE;  // table full
}

int FDTable::dup2(int oldfd, int newfd) {
    FileRef displaced;
    Inode*  disp_inode = nullptr;
    {
        auto g = lock_.guard();

        if (oldfd < 0 || oldfd >= static_cast<int>(FD_TABLE_SIZE) || !fds_[oldfd]) {
            return FD_NONE;
        }
        if (newfd < 0 || newfd >= static_cast<int>(FD_TABLE_SIZE)) {
            return FD_NONE;
        }
        if (oldfd == newfd) {
            return newfd;  // Linux: valid + same -> no-op, return newfd
        }
        if (fds_[newfd]) {
            // Detach the old occupant; finalize outside the lock so its
            // InodeOps::release cannot block the fd table (DEBT-023).
            disp_inode = fds_[newfd]->inode;
            displaced  = std::move(fds_[newfd]);  // newfd becomes empty
        }
        // Batch 1: still an INDEPENDENT copy. Batch 2 shares the File.
        File* src   = fds_[oldfd].get();
        fds_[newfd] = FileRef(new File(src->inode, src->offset, src->flags, src->cloexec));
        inode_ref(src->inode);  // DEBT-023: the duplicate holds its own ref
    }
    inode_unref(disp_inode);  // old occupant's reference, last-close release (null-safe)
    return newfd;
    // ~displaced (outside lock): FileRef unref the displaced File (delete on 0)
}

}  // namespace cinux::fs
