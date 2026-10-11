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

BlockCache::BlockCache(BlockQueue& queue, unsigned long slot_sectors, unsigned long slot_count)
    : queue_(queue), slot_sectors_(slot_sectors), slot_bytes_(slot_sectors * kSectorBytes) {
    const unsigned long kWanted = base::math::Min(slot_count, kBlockCacheSlots);
    for (unsigned long index = 0; index < kWanted; ++index) {
        auto* const         kFresh = new uint8_t[slot_bytes_]{};
        const unsigned long kTaken =
            slots_.insert(Slot{.first = Lba{}, .live = true, .bytes = kFresh});
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
    const Slot* const      kSlot = claim_slot(first);
    if (kSlot == nullptr) {
        return false;
    }
    *view_out = kSlot->bytes;
    return true;
}

bool BlockCache::patch(Lba first, unsigned long offset, unsigned long bytes, const void* source) {
    if (offset + bytes > slot_bytes_) {
        return false;
    }
    const proc::MutexGuard kGate(gate_);
    Slot* const            kSlot = claim_slot(first);
    if (kSlot == nullptr) {
        return false;
    }
    base::CopyBytes(kSlot->bytes + offset, source, bytes);
    const BlockSpan kWhole{.value = static_cast<uint16_t>(slot_sectors_)};
    if (!queue_.write(kSlot->first, kWhole, kSlot->bytes)) {
        kSlot->live = false;
        return false;
    }
    return true;
}

unsigned long BlockCache::slot_count() const {
    return slot_total_;
}

unsigned long BlockCache::slot_sectors() const {
    return slot_sectors_;
}

BlockCache::Slot* BlockCache::claim_slot(Lba first) {
    const unsigned long kTotal = slot_total_;
    if (kTotal == 0) {
        return nullptr;
    }
    const unsigned long kHit = slots_.find_if(
        [&first](const Slot& slot) { return slot.live && slot.first.value == first.value; });
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
    kVictim->first = first;
    kVictim->live  = false;
    const BlockSpan kWhole{.value = static_cast<uint16_t>(slot_sectors_)};
    if (!queue_.read(first, kWhole, kVictim->bytes)) {
        return nullptr;
    }
    kVictim->live = true;
    return kVictim;
}

}  // namespace cinux::driver
