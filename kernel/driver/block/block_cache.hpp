/**
 * @file    block_cache.hpp
 * @brief   The block cache: fixed write-through slots over the queue.
 *
 * A file system asks for the same metadata blocks again and again —
 * bitmaps, inode tables, the superblock — and each ask through the
 * queue costs a device command. The cache keeps a handful of slots,
 * each covering a stretch of consecutive sectors, and answers those
 * repeats from memory. It is write-through by construction: a patch
 * lands in the slot and on the disk in one breath, so a live slot
 * always matches the platter and reads can trust what they see. A
 * fill or write the device refuses leaves the slot dead — out of
 * the answers until a later claim refills it — rather than serving
 * bytes the platter never took. The lock holds across those queue
 * commands on purpose (a miss sleeps on the queue), so the roster
 * never moves under a caller's feet; a slot filled twice with the
 * same bytes is harmless, while overlapping patches are the
 * caller's promise to serialize — the file-system write path holds
 * its mutex across a whole metadata sequence, which is exactly that
 * serialization.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/container/slot_table.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {

class BlockQueue;

/**
 * @brief   Fixed-slot write-through cache over one block queue.
 * @note    Slots are sized at construction in whole sectors; a
 *          consumer whose blocks span several sectors asks once per
 *          slot-sized stretch. Replacement is a plain round robin —
 *          a teaching kernel favours obvious over clever, and the
 *          real thing's LRU is one station's text, not its code.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
class BlockCache {
public:
    /**
     * @brief         Binds the cache to its queue and shape.
     *
     * @param[in,out] queue   The queue serving the device; task
     *                        contexts only, per the queue's contract.
     * @param[in]     slot_sectors   Sectors one slot covers; zero
     *                                makes every ask refuse.
     * @param[in]     slot_count     Slots to keep, capped at
     *                                kBlockCacheSlots; zero makes
     *                                every ask refuse.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    BlockCache(BlockQueue& queue, unsigned long slot_sectors, unsigned long slot_count);

    ~BlockCache();

    BlockCache(const BlockCache&)            = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    /**
     * @brief         Hands out a read view of one slot.
     *
     * @param[in]     first   The slot's first sector.
     * @param[out]    view    Slot bytes, live until the next roster
     *                        change; copy out anything that must
     *                        survive.
     * @return        True with view filled; false when the device
     *                refused the fill.
     * @note          Task contexts only: a miss sleeps on the queue.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool view(Lba first, const uint8_t** view);

    /**
     * @brief         Writes bytes into a slot and through to disk.
     *
     * @param[in]     first    The slot's first sector.
     * @param[in]     offset   Byte offset inside the slot.
     * @param[in]     bytes    Bytes to store.
     * @param[in]     source   Bytes to store from.
     * @return        True when slot and disk both took the change;
     *                false on a device refusal.
     * @warning       Overlapping patches from concurrent callers are
     *                not serialized here; the write path's mutex
     *                owns that promise.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool patch(Lba first, unsigned long offset, unsigned long bytes,
                             const void* source);

    /// @brief How many slots the cache keeps.
    [[nodiscard]] unsigned long slot_count() const;

    /// @brief How many sectors one slot covers.
    [[nodiscard]] unsigned long slot_sectors() const;

private:
    /**
     * @brief   One cached stretch of sectors.
     * @note    The bytes live on the heap so the roster element
     *          stays small; the destructor owns every allocation.
     * @since   0.1.0
     * @ingroup kernel_driver
     */
    struct Slot {
        Lba      first{};          ///< The stretch's first sector.
        bool     live  = false;    ///< Whether the bytes are filled.
        uint8_t* bytes = nullptr;  ///< The stretch's bytes.
    };

    [[nodiscard]] Slot* claim_slot(Lba first);

    BlockQueue& queue_;

    unsigned long slot_sectors_ = 0;
    unsigned long slot_bytes_   = 0;
    unsigned long slot_total_   = 0;

    base::container::SlotTable<Slot, kBlockCacheSlots> slots_;
    unsigned long                                      next_ = 0;

    proc::Mutex gate_;
};

}  // namespace cinux::driver
