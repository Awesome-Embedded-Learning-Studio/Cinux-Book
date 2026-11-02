/**
 * @file    block_cache.hpp
 * @brief   The block cache: fixed page-grain slots over the queue.
 *
 * A file system asks for the same blocks again and again — bitmaps,
 * inode tables, the superblock, and now whole file pages on the fault
 * path — and each ask through the queue costs a device command. The
 * cache keeps a handful of slots, each covering one 4 KiB page of the
 * platter, and answers those repeats from memory. A caller still
 * speaks in sectors: the cache translates an ask into the page that
 * holds it and hands back a pointer into the slot's middle. Writes
 * land in the slot alone and mark it dirty; the platter catches up
 * when that page is evicted or when flush is called, so one patch
 * costs no device command and a burst of patches into one page costs
 * a single write. The price is that a live slot may run ahead of the
 * disk: a reader outside the cache — a test peeking at the image, or
 * a crash — sees the older bytes until flush. A write-back the device
 * refuses leaves the victim in place and still dirty: the bytes stay
 * answerable from memory, and the page is retried at the next
 * eviction or flush rather than silently dropped. A fill the device
 * refuses leaves the slot dead — out of the answers until a later
 * claim refills it — rather than serving bytes the platter never
 * took. The lock holds across those queue commands on purpose (a
 * miss or a spill sleeps on the queue), so the roster never moves
 * under a caller's feet; a slot filled twice with the same bytes is
 * harmless, while overlapping patches are the caller's promise to
 * serialize — the file-system write path holds its mutex across a
 * whole metadata sequence, which is exactly that serialization.
 *
 * @author  Charliechen114514
 * @date    2026-10-15
 * @version 0.4
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/container/self_list.hpp"
#include "cinux/own.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {

class BlockQueue;

/**
 * @brief   Fixed-slot page-grain cache over one block queue.
 * @note    Every slot covers kCachePageSectors sectors — one 4 KiB
 *          page — and the slot's key is that page's number, the
 *          sector count divided down; sector asks and page keys are
 *          two names for one platter fact, so the translation is
 *          lossless in both directions. A hash index locates resident
 *          pages; second-chance replacement preserves recently used pages.
 *          Data buffers are allocated on demand within the memory budget.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
class BlockCache {
public:
    /**
     * @brief Private page copies published together after all destination pages are pinned.
     * @note A failed preparation or commit leaves the cache's logical bytes unchanged.
     *       The caller serializes competing changes to the same filesystem.
     */
    class Batch {
    public:
        /// Bind a new, unpublished batch to its cache.
        explicit Batch(BlockCache& cache) : cache_(cache) {}
        ~Batch();
        Batch(const Batch&)                        = delete;
        Batch&             operator=(const Batch&) = delete;
        /// Read staged bytes when present, otherwise read the original cache.
        [[nodiscard]] bool read(Lba first, void* sink, unsigned long bytes);
        /// Change a private page copy; failures poison the whole batch.
        [[nodiscard]] bool patch(Lba first, unsigned long offset, unsigned long bytes,
                                 const void* source);
        /// Publish all prepared copies without device I/O during publication.
        [[nodiscard]] bool commit();

    private:
        friend class BlockCache;
        struct Page : base::container::SelfNode<Page> {
            unsigned long        number = 0;
            unsigned long        slot   = ~0UL;
            base::Own<uint8_t[]> bytes;
        };
        Page*                           find(unsigned long number);
        void                            unpin();
        BlockCache&                     cache_;
        base::container::SelfList<Page> pages_;
        bool                            failed_ = false;
    };

    /**
     * @brief         Binds the cache to its queue and shape.
     *
     * @param[in,out] queue   The queue serving the device; task
     *                        contexts only, per the queue's contract.
     * @param[in]     slot_count     Slots to keep, capped at
     *                                kBlockCacheMaxBytes; zero makes
     *                                every ask refuse.
     * @since         0.3.0
     * @ingroup       kernel_driver
     */
    BlockCache(BlockQueue& queue, unsigned long slot_count);

    ~BlockCache();

    BlockCache(const BlockCache&)            = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    /**
     * @brief         Hands out a read view starting at one sector.
     *
     * @param[in]     first   The first sector the caller wants; the
     *                        page holding it is filled as a whole.
     * @param[out]    view    Bytes from that sector on, live until
     *                        the next roster change; copy out
     *                        anything that must survive.
     * @return        True with view filled; false when the device
     *                refused the fill.
     * @note          Task contexts only: a miss sleeps on the queue.
     *                Concurrent callers must use read() to keep the
     *                copy inside the lock; this borrowed view is not pinned.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool view(Lba first, const uint8_t** view);

    /**
     * @brief         Copies cached bytes while the slot remains locked.
     * @param[in]     first   First sector of the requested bytes.
     * @param[out]    sink    Caller-owned destination.
     * @param[in]     bytes   Byte count, contained in one cache page.
     * @return        False on a crossing request or a failed fill.
     * @note          Task contexts only; safe across concurrent readers.
     * @since         0.5.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool read(Lba first, void* sink, unsigned long bytes);

    /**
     * @brief         Writes bytes into a slot and marks it dirty.
     *
     * @param[in]     first    The first sector the change touches.
     * @param[in]     offset   Byte offset from that sector.
     * @param[in]     bytes    Bytes to store.
     * @param[in]     source   Bytes to store from.
     * @return        True when the slot took the change; false when
     *                the change would cross out of its page. No
     *                device command runs here — the page reaches the
     *                platter at eviction or flush.
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

    /**
     * @brief         Writes every dirty page and waits for the device flush.
     *
     * @return        True when every write and the device flush succeeded.
     *                Refused writes remain dirty; a failed device flush
     *                is retried even when no dirty pages remain.
     * @note          Task contexts only: the writes sleep on the
     *                queue.
     * @since         0.4.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool flush();

private:
    /**
     * @brief   One cached page of the platter.
     * @note    The bytes live on the heap so the roster element
     *          stays small; the destructor owns every allocation.
     * @since   0.1.0
     * @ingroup kernel_driver
     */
    struct Slot {
        unsigned long page{};                ///< The platter page these bytes hold.
        bool          live       = false;    ///< Whether the bytes are filled.
        bool          dirty      = false;    ///< Whether the platter runs behind these bytes.
        uint8_t*      bytes      = nullptr;  ///< The page's bytes.
        unsigned long next_hash  = ~0UL;     ///< Next slot in this hash chain.
        bool          pinned     = false;
        bool          referenced = false;  ///< Accessed since the replacement hand passed.
    };

    [[nodiscard]] Slot*         claim_slot(unsigned long page);
    [[nodiscard]] unsigned long find_slot(unsigned long page) const;
    [[nodiscard]] unsigned long choose_victim();
    void                        remove_index(unsigned long index);

    /**
     * @brief         Writes one live slot's bytes to its page.
     * @param[in,out] slot   The slot to spill; dirty clears on
     *                       success and stands on a refusal.
     * @return        True when the platter took the page.
     * @since         0.4.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool spill(Slot& slot);

    BlockQueue& queue_;

    unsigned long slot_bytes_ = 0;
    unsigned long slot_total_ = 0;

    base::Own<Slot[]>          slots_;
    base::Own<unsigned long[]> buckets_;
    unsigned long              bucket_count_ = 0;
    unsigned long              next_         = 0;

    proc::Mutex gate_;
};

}  // namespace cinux::driver
