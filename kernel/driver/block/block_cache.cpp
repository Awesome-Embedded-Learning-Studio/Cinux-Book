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
#include "cinux/own.hpp"
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
    slot_total_ = base::math::Min(slot_count, kBlockCacheMaxBytes / slot_bytes_);
    if (slot_total_ == 0) {
        return;
    }
    slots_.reset(new Slot[slot_total_]{});
    bucket_count_ = (slot_total_ * 2) + 1;
    buckets_.reset(new unsigned long[bucket_count_]);
    for (unsigned long index = 0; index < bucket_count_; ++index) {
        buckets_[index] = ~0UL;
    }
}

BlockCache::~BlockCache() {
    for (unsigned long index = 0; index < slot_total_; ++index) {
        delete[] slots_[index].bytes;
    }
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

bool BlockCache::read(Lba first, void* sink, unsigned long bytes) {
    const unsigned long kOffset = offset_of(first);
    if (bytes > slot_bytes_ - kOffset) {
        return false;
    }
    const proc::MutexGuard kGate(gate_);
    const Slot* const      kSlot = claim_slot(page_of(first));
    if (kSlot == nullptr) {
        return false;
    }
    base::CopyBytes(sink, kSlot->bytes + kOffset, bytes);
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
    for (unsigned long index = 0; index < slot_total_; ++index) {
        Slot& slot = slots_[index];
        if (slot.live && slot.dirty && !spill(slot)) {
            all = false;
        }
    }
    return all;
}

unsigned long BlockCache::slot_count() const {
    return slot_total_;
}

unsigned long BlockCache::slot_sectors() const {
    return kCachePageSectors;
}

unsigned long BlockCache::find_slot(unsigned long page) const {
    for (unsigned long index = buckets_[page % bucket_count_]; index != ~0UL;
         index               = slots_[index].next_hash) {
        if (slots_[index].page == page) {
            return index;
        }
    }
    return ~0UL;
}

unsigned long BlockCache::choose_victim() {
    for (;;) {
        const unsigned long kIndex = next_;
        next_                      = (next_ + 1) % slot_total_;
        Slot& candidate            = slots_[kIndex];
        if (!candidate.live || !candidate.referenced) {
            return kIndex;
        }
        candidate.referenced = false;
    }
}

void BlockCache::remove_index(unsigned long index) {
    unsigned long* link = &buckets_[slots_[index].page % bucket_count_];
    while (*link != index) {
        link = &slots_[*link].next_hash;
    }
    *link                   = slots_[index].next_hash;
    slots_[index].next_hash = ~0UL;
}

BlockCache::Slot* BlockCache::claim_slot(unsigned long page) {
    if (slot_total_ == 0) {
        return nullptr;
    }
    const unsigned long kHit = find_slot(page);
    if (kHit != ~0UL) {
        slots_[kHit].referenced = true;
        return &slots_[kHit];
    }
    const unsigned long kIndex = choose_victim();
    Slot&               victim = slots_[kIndex];
    if (victim.live && victim.dirty && !spill(victim)) {
        return nullptr;
    }
    if (victim.live) {
        remove_index(kIndex);
    }
    victim.live  = false;
    victim.dirty = false;
    if (victim.bytes == nullptr) {
        victim.bytes = new uint8_t[slot_bytes_]{};
    }
    const Lba       kPageStart{.value = page * kCachePageSectors};
    const BlockSpan kWhole{.value = kCachePageSectors};
    if (!queue_.read(kPageStart, kWhole, victim.bytes)) {
        return nullptr;
    }
    victim.page        = page;
    victim.live        = true;
    victim.referenced  = true;
    const auto kBucket = page % bucket_count_;
    victim.next_hash   = buckets_[kBucket];
    buckets_[kBucket]  = kIndex;
    return &victim;
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
