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
    for (uint32_t i = 0; i < FD_TABLE_SIZE; ++i) {
        fds_[i] = nullptr;
    }
}

// ============================================================
// Destruction (resource-safety backstop)
// ============================================================

FDTable::~FDTable() {
    // Free any File that was never close()'d. delete on nullptr is a safe
    // no-op, so under the normal release() path (which closes every slot
    // before `delete this`) this loop touches only nullptrs. It catches
    // stack-allocated tables and skip-release paths so no File leaks (DEBT-017).
    for (uint32_t i = 0; i < FD_TABLE_SIZE; ++i) {
        delete fds_[i];
    }
}

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
            if (fds_[i] != nullptr) {
                close(static_cast<int>(i));
            }
        }
        delete this;
    }
}

// ============================================================
// Alloc
// ============================================================

/// First assignable fd (0=stdin, 1=stdout, 2=stderr are reserved)
static constexpr uint32_t FD_FIRST = 3;

int FDTable::alloc(Inode* inode, OpenFlags flags) {
    auto g = lock_.guard();
    (void)g;

    for (uint32_t i = FD_FIRST; i < FD_TABLE_SIZE; ++i) {
        if (fds_[i] == nullptr) {
            fds_[i] = new File(inode, 0, flags);
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
    File*  file  = nullptr;
    Inode* inode = nullptr;
    {
        auto g = lock_.guard();
        (void)g;

        if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
            return -1;
        }
        if (fds_[fd] == nullptr) {
            return -1;
        }
        // Detach under the lock; destroy + unref OUTSIDE so InodeOps::release
        // (a socket FIN, a pipe close_writer waking blocked peers) cannot block
        // the fd table.  File is a plain struct (no inode touch on destroy), so
        // deleting it is always safe; inode_unref does the refcount + last-close
        // release (DEBT-023).
        file     = fds_[fd];
        inode    = file->inode;
        fds_[fd] = nullptr;
    }
    delete file;         // plain struct: no inode touch
    inode_unref(inode);  // --refcount; on last close -> InodeOps::release
    return 0;
}

// ============================================================
// Get
// ============================================================

File* FDTable::get(int fd) const {
    auto g = lock_.guard();
    (void)g;

    if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
        return nullptr;
    }
    return fds_[fd];
}

// ============================================================
// Set
// ============================================================

bool FDTable::set(int fd, File* file) {
    auto g = lock_.guard();
    (void)g;

    if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
        return false;
    }
    // The caller owns any previous occupant (header doc); the incoming file
    // gains an inode reference here (DEBT-023), paired with close()'s unref.
    fds_[fd] = file;
    if (file != nullptr) {
        inode_ref(file->inode);
    }
    return true;
}

// ============================================================
// Dup / Dup2 (F-ECO batch 4)
// ============================================================

int FDTable::dup(int oldfd, int min_fd) {
    auto g = lock_.guard();
    (void)g;

    if (oldfd < 0 || oldfd >= static_cast<int>(FD_TABLE_SIZE) || fds_[oldfd] == nullptr) {
        return FD_NONE;
    }
    // dup never hands out the reserved stdin/out/err slots (0/1/2): start at
    // FD_FIRST.  This is a hobby-OS deviation from Linux (which dup's lowest >=
    // 0); it keeps tests deterministic and avoids clobbering the console fds.
    int start = min_fd < static_cast<int>(FD_FIRST) ? static_cast<int>(FD_FIRST) : min_fd;
    for (int i = start; i < static_cast<int>(FD_TABLE_SIZE); ++i) {
        if (fds_[i] == nullptr) {
            File* src = fds_[oldfd];
            fds_[i]   = new File(src->inode, src->offset, src->flags, src->cloexec);
            inode_ref(src->inode);  // DEBT-023: the duplicate holds its own ref
            return i;
        }
    }
    return FD_NONE;  // table full
}

int FDTable::dup2(int oldfd, int newfd) {
    File*  displaced  = nullptr;
    Inode* disp_inode = nullptr;
    {
        auto g = lock_.guard();
        (void)g;

        if (oldfd < 0 || oldfd >= static_cast<int>(FD_TABLE_SIZE) || fds_[oldfd] == nullptr) {
            return FD_NONE;
        }
        if (newfd < 0 || newfd >= static_cast<int>(FD_TABLE_SIZE)) {
            return FD_NONE;
        }
        if (oldfd == newfd) {
            return newfd;  // Linux: valid + same -> no-op, return newfd
        }
        if (fds_[newfd] != nullptr) {
            // Detach the old occupant; finalize outside the lock so its
            // InodeOps::release cannot block the fd table (DEBT-023).
            displaced   = fds_[newfd];
            disp_inode  = displaced->inode;
            fds_[newfd] = nullptr;
        }
        File* src   = fds_[oldfd];
        fds_[newfd] = new File(src->inode, src->offset, src->flags, src->cloexec);
        inode_ref(src->inode);  // DEBT-023: the duplicate holds its own ref
    }
    delete displaced;         // plain struct: no inode touch (no-op if null)
    inode_unref(disp_inode);  // old occupant's reference, last-close release (null-safe)
    return newfd;
}

}  // namespace cinux::fs
