/**
 * @file    block_cache.cpp
 * @brief   Roster, fill, and patch: the cache's moving parts.
 *
 * @author  Charliechen114514
 * @date    2026-10-10
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/block/block_cache.hpp"

#include <cstdint>

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {

namespace {

/// The page a sector ask lands in, and where inside that page it sits.
unsigned long page_of(Lba sector) {
    return sector.value / kCachePageSectors;
}

unsigned long offset_of(Lba sector) {
    return (sector.value % kCachePageSectors) * kSectorBytes;
}

}  // namespace

BlockCache::BlockCache(BlockQueue& queue, unsigned long slot_count)
    : queue_(queue), slot_bytes_(kCachePageSectors * kSectorBytes) {
    const unsigned long kWanted = base::math::Min(slot_count, kBlockCacheSlots);
    for (unsigned long index = 0; index < kWanted; ++index) {
        auto* const         kFresh = new uint8_t[slot_bytes_]{};
        const unsigned long kTaken = slots_.insert(Slot{.page = 0, .live = false, .bytes = kFresh});
        if (kTaken == kBlockCacheSlots) {
            delete[] kFresh;
        }
    }
    slot_total_ = kWanted;
}

BlockCache::~BlockCache() {
    slots_.for_each([](unsigned long, Slot& slot) { delete[] slot.bytes; });
}

bool BlockCache::view(Lba first, const uint8_t** view_out) {
    const proc::MutexGuard kGate(gate_);
    const Slot* const      kSlot = claim_slot(page_of(first));
    if (kSlot == nullptr) {
        return false;
    }
    *view_out = kSlot->bytes + offset_of(first);
    return true;
}

bool BlockCache::patch(Lba first, unsigned long offset, unsigned long bytes, const void* source) {
    const unsigned long kInPage = offset_of(first);
    if (kInPage + offset + bytes > slot_bytes_) {
        return false;
    }
    const proc::MutexGuard kGate(gate_);
    Slot* const            kSlot = claim_slot(page_of(first));
    if (kSlot == nullptr) {
        return false;
    }
    base::CopyBytes(kSlot->bytes + kInPage + offset, source, bytes);
    kSlot->dirty = true;
    return true;
}

bool BlockCache::flush() {
    const proc::MutexGuard kGate(gate_);
    bool                   all = true;
    slots_.for_each([this, &all](unsigned long, Slot& slot) {
        if (slot.live && slot.dirty && !spill(slot)) {
            all = false;
        }
    });
    return all;
}

unsigned long BlockCache::slot_count() const {
    return slot_total_;
}

unsigned long BlockCache::slot_sectors() const {
    return kCachePageSectors;
}

BlockCache::Slot* BlockCache::claim_slot(unsigned long page) {
    const unsigned long kTotal = slot_total_;
    if (kTotal == 0) {
        return nullptr;
    }
    const unsigned long kHit =
        slots_.find_if([page](const Slot& slot) { return slot.live && slot.page == page; });
    if (const Slot* const kFound = slots_.get(kHit);
        kFound != nullptr && kFound->bytes != nullptr) {
        return slots_.get(kHit);
    }

    unsigned long victim = next_;
    for (unsigned long step = 0; step < kTotal; ++step) {
        const unsigned long kIndex = (next_ + step) % kTotal;
        if (const Slot* const kCandidate = slots_.get(kIndex);
            kCandidate != nullptr && kCandidate->bytes != nullptr) {
            victim = kIndex;
            break;
        }
    }
    next_ = (victim + 1) % kTotal;

    Slot* const kVictim = slots_.get(victim);
    if (kVictim == nullptr) {
        return nullptr;
    }
    if (kVictim->live && kVictim->dirty && !spill(*kVictim)) {
        return nullptr;
    }
    const Lba       kPageStart{.value = page * kCachePageSectors};
    const BlockSpan kWhole{.value = kCachePageSectors};
    kVictim->page  = page;
    kVictim->live  = false;
    kVictim->dirty = false;
    if (!queue_.read(kPageStart, kWhole, kVictim->bytes)) {
        return nullptr;
    }
    kVictim->live = true;
    return kVictim;
}

bool BlockCache::spill(Slot& slot) {
    const Lba       kPageStart{.value = slot.page * kCachePageSectors};
    const BlockSpan kWhole{.value = kCachePageSectors};
    if (!queue_.write(kPageStart, kWhole, slot.bytes)) {
        return false;
    }
    slot.dirty = false;
    return true;
}

}  // namespace cinux::driver
