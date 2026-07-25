/**
 * @file kernel/ipc/shm.cpp
 * @brief SysV shared memory registry implementation (F8-M4)
 *
 * Pure table logic only -- no kprintf, no PMM, no AddressSpace -- so this TU
 * links cleanly into host unit tests exactly like fifo.cpp.  The physical-page
 * lifecycle (alloc_pages / free_pages / pte_count) lives in sys_shm.cpp; this
 * file just owns the key -> segment bookkeeping and the attach / removal state
 * machine.
 */

#include "kernel/ipc/shm.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/lib/string.hpp"  // memset (stat)

namespace cinux::ipc {

// ============================================================
// ShmRegistry
// ============================================================

ShmRegistry& ShmRegistry::instance() {
    static ShmRegistry reg;
    return reg;
}

int ShmRegistry::find_key_locked(int key) const {
    // IPC_PRIVATE is "always create" and never matches an existing entry.
    if (key == kIpcPrivate) {
        return -1;
    }
    for (uint32_t i = 0; i < kShmRegistryMax; ++i) {
        if (entries_[i].used && entries_[i].seg.key == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ShmRegistry::find_phys_locked(uint64_t phys_base) const {
    for (uint32_t i = 0; i < kShmRegistryMax; ++i) {
        if (entries_[i].used && entries_[i].seg.phys_base == phys_base) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ShmRegistry::find_free_locked() const {
    for (uint32_t i = 0; i < kShmRegistryMax; ++i) {
        if (!entries_[i].used) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

cinux::lib::ErrorOr<int> ShmRegistry::find_by_key(int key) const {
    auto g = lock_.guard();
    int i = find_key_locked(key);
    if (i < 0) {
        return cinux::lib::Error::NotFound;
    }
    return i;
}

cinux::lib::ErrorOr<int> ShmRegistry::add(int key, uint64_t size, uint64_t phys_base,
                                          uint64_t page_count, uint16_t mode) {
    auto g = lock_.guard();
    int i = find_free_locked();
    if (i < 0) {
        return cinux::lib::Error::OutOfMemory;  // table full
    }
    ShmSegment& s        = entries_[i].seg;
    s.used               = true;  // marks the slot occupied (segment.used)
    s.key                = key;
    s.phys_base          = phys_base;
    s.page_count         = page_count;
    s.size               = size;
    s.mode               = mode;
    s.nattach            = 0;
    s.cpid               = 0;  // syscall layer stamps cpid/lpid from the task
    s.lpid               = 0;
    s.marked_for_removal = false;
    entries_[i].used     = true;
    return i;
}

ShmSegment* ShmRegistry::segment(int shmid) {
    if (shmid < 0 || shmid >= static_cast<int>(kShmRegistryMax)) {
        return nullptr;
    }
    if (!entries_[shmid].used) {
        return nullptr;
    }
    return &entries_[shmid].seg;
}

cinux::lib::ErrorOr<ShmSegment> ShmRegistry::attach(int shmid, uint32_t tid) {
    auto g = lock_.guard();
    if (shmid < 0 || shmid >= static_cast<int>(kShmRegistryMax) || !entries_[shmid].used) {
        return cinux::lib::Error::NotFound;
    }
    ShmSegment& s = entries_[shmid].seg;
    // A marked segment is being torn down; refuse a new attachment.
    if (s.marked_for_removal) {
        return cinux::lib::Error::NotFound;
    }
    ++s.nattach;
    s.lpid = tid;
    return s;  // returns a snapshot copy
}

uint64_t ShmRegistry::detach(int shmid, uint32_t tid, uint64_t* out_count) {
    if (out_count != nullptr) {
        *out_count = 0;
    }
    auto g = lock_.guard();
    if (shmid < 0 || shmid >= static_cast<int>(kShmRegistryMax) || !entries_[shmid].used) {
        return 0;  // stale shmdt: benign no-op
    }
    ShmSegment& s = entries_[shmid].seg;
    if (s.nattach > 0) {
        --s.nattach;
    }
    s.lpid = tid;
    // Free only when a marked segment has lost its last attachment.
    if (s.marked_for_removal && s.nattach == 0) {
        uint64_t phys = s.phys_base;
        if (out_count != nullptr) {
            *out_count = s.page_count;
        }
        // Clear the slot so a future shmget can reuse it.
        entries_[shmid].used = false;
        s.used               = false;
        s.marked_for_removal = false;
        s.phys_base          = 0;
        s.page_count         = 0;
        s.nattach            = 0;
        return phys;
    }
    return 0;
}

uint64_t ShmRegistry::mark_removal(int shmid, uint64_t* out_count) {
    if (out_count != nullptr) {
        *out_count = 0;
    }
    auto g = lock_.guard();
    if (shmid < 0 || shmid >= static_cast<int>(kShmRegistryMax) || !entries_[shmid].used) {
        return 0;  // already gone: report nothing to free
    }
    ShmSegment& s        = entries_[shmid].seg;
    s.marked_for_removal = true;
    // No live attachments: tear down immediately and hand the pages back.
    if (s.nattach == 0) {
        uint64_t phys = s.phys_base;
        if (out_count != nullptr) {
            *out_count = s.page_count;
        }
        entries_[shmid].used = false;
        s.used               = false;
        s.marked_for_removal = false;
        s.phys_base          = 0;
        s.page_count         = 0;
        return phys;
    }
    return 0;  // lingers until the last detach frees it
}

cinux::lib::ErrorOr<void> ShmRegistry::stat(int shmid, shmid_ds* out) const {
    if (out == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    if (shmid < 0 || shmid >= static_cast<int>(kShmRegistryMax) || !entries_[shmid].used) {
        return cinux::lib::Error::NotFound;
    }
    const ShmSegment& s = entries_[shmid].seg;
    memset(out, 0, sizeof(*out));
    out->shm_segsz  = s.size;
    out->shm_cpid   = s.cpid;
    out->shm_lpid   = s.lpid;
    out->shm_nattch = s.nattach;
    out->shm_mode   = s.mode;
    return {};
}

cinux::lib::ErrorOr<int> ShmRegistry::find_by_phys(uint64_t phys_base) const {
    auto g = lock_.guard();
    int i = find_phys_locked(phys_base);
    if (i < 0) {
        return cinux::lib::Error::NotFound;
    }
    return i;
}

}  // namespace cinux::ipc
