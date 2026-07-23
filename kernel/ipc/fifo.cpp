/**
 * @file kernel/ipc/fifo.cpp
 * @brief Named FIFO implementation: FifoRegistry + FifoOps cloning open (F8-M2)
 *
 * Pure logic only -- no kprintf, no kernel-only I/O -- so this translation unit
 * links cleanly into the host unit tests (the Pipe it builds is also
 * host-safe: the blocking path is compiled out under CINUX_HOST_TEST).
 */

#include "kernel/ipc/fifo.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/lib/string.hpp"  // memset (stat)

namespace cinux::ipc {

// ============================================================
// Fifo
// ============================================================

Pipe* Fifo::get_or_create_pipe() {
    auto g = lock.guard();
    if (pipe == nullptr) {
        pipe = new Pipe();
    }
    return pipe;
}

// ============================================================
// FifoRegistry
// ============================================================

FifoRegistry& FifoRegistry::instance() {
    static FifoRegistry reg;
    return reg;
}

int FifoRegistry::find_locked(const char* name) const {
    if (name == nullptr) {
        return -1;
    }
    for (uint32_t i = 0; i < FIFO_REGISTRY_MAX; ++i) {
        if (!entries_[i].used) {
            continue;
        }
        const char* a = entries_[i].name;
        uint32_t    j = 0;
        while (a[j] != '\0' && name[j] != '\0') {
            if (a[j] != name[j]) {
                break;
            }
            ++j;
        }
        if (a[j] == '\0' && name[j] == '\0') {
            return static_cast<int>(i);
        }
    }
    return -1;
}

cinux::lib::ErrorOr<void> FifoRegistry::create(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    if (find_locked(name) >= 0) {
        return cinux::lib::Error::AlreadyExists;
    }
    for (uint32_t i = 0; i < FIFO_REGISTRY_MAX; ++i) {
        if (!entries_[i].used) {
            uint32_t j = 0;
            while (j + 1 < FIFO_NAME_MAX && name[j] != '\0') {
                entries_[i].name[j] = name[j];
                ++j;
            }
            entries_[i].name[j]          = '\0';
            entries_[i].used             = true;
            entries_[i].fifo.pipe        = nullptr;  // lazy: created on first open
            // Stable FIFO inode for DevFS lookup: shared FifoOps + the entry's
            // Fifo as per-FIFO state (fs_private).
            entries_[i].inode            = cinux::fs::Inode{};
            entries_[i].inode.ino        = kFifoInoBase + i;
            entries_[i].inode.type       = cinux::fs::InodeType::Regular;
            entries_[i].inode.ops        = &fifo_ops();
            entries_[i].inode.fs_private = &entries_[i].fifo;
            entries_[i].inode.mode       = kSIfFifo | 0666;
            return {};
        }
    }
    return cinux::lib::Error::OutOfMemory;  // table full
}

cinux::lib::ErrorOr<Fifo*> FifoRegistry::lookup(const char* name) {
    if (name == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    int  i = find_locked(name);
    if (i < 0) {
        return cinux::lib::Error::NotFound;
    }
    return &entries_[i].fifo;
}

cinux::lib::ErrorOr<cinux::fs::Inode*> FifoRegistry::lookup_inode(const char* name) {
    if (name == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    int  i = find_locked(name);
    if (i < 0) {
        return cinux::lib::Error::NotFound;
    }
    return &entries_[i].inode;
}

void FifoRegistry::remove(const char* name) {
    auto g = lock_.guard();
    int  i = find_locked(name);
    if (i < 0) {
        return;
    }
    if (entries_[i].fifo.pipe != nullptr) {
        delete entries_[i].fifo.pipe;
        entries_[i].fifo.pipe = nullptr;
    }
    entries_[i].used    = false;
    entries_[i].name[0] = '\0';
}

// ============================================================
// FifoOps
// ============================================================

cinux::lib::ErrorOr<cinux::fs::Inode*> FifoOps::open(cinux::fs::Inode* inode, uint64_t flags) {
    if (inode == nullptr || inode->fs_private == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto* fifo = static_cast<Fifo*>(inode->fs_private);
    Pipe* pipe = fifo->get_or_create_pipe();  // first open lazily builds it
    if (pipe == nullptr) {
        return cinux::lib::Error::OutOfMemory;
    }

    bool is_write = (flags & kOAccessMask) == kOWronly;
    bool nonblock = (flags & kONonblock) != 0;

    // Per-open inode bound to one end of the shared pipe.  Leaked on close
    // (no InodeOps::release hook) -- the same hobby-OS limitation as anonymous
    // pipes (see sys_pipe: closing a fd frees the File but not the ops/pipe).
    auto* end = new cinux::fs::Inode();
    if (end == nullptr) {
        return cinux::lib::Error::OutOfMemory;
    }
    end->type = cinux::fs::InodeType::Regular;
    end->ops  = is_write ? static_cast<cinux::fs::InodeOps*>(new PipeWriteOps(pipe, nonblock))
                         : static_cast<cinux::fs::InodeOps*>(new PipeReadOps(pipe, nonblock));
    if (end->ops == nullptr) {
        delete end;
        return cinux::lib::Error::OutOfMemory;
    }
    return end;
}

cinux::lib::ErrorOr<void> FifoOps::stat(const cinux::fs::Inode* inode, cinux::fs::stat* st) {
    if (inode == nullptr || st == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    memset(st, 0, sizeof(*st));
    st->st_ino     = inode->ino;
    st->st_nlink   = 1;
    st->st_mode    = kSIfFifo | 0666;
    st->st_blksize = 4096;
    return {};
}

FifoOps& fifo_ops() {
    static FifoOps inst;
    return inst;
}

}  // namespace cinux::ipc
