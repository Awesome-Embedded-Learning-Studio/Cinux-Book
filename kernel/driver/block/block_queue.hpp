/**
 * @file    block_queue.hpp
 * @brief   The request queue: many callers, one device, one duty roster.
 *
 * A block device answers one command at a time, while any number of
 * tasks may want it at once — the queue is where those two facts meet.
 * Callers park requests and sleep on each one's completion; whoever
 * finds the device idle takes the duty and drives every parked request
 * to completion before laying the duty down, so a sleeper wakes to find
 * its data moved and the device never sees two commands overlap. The
 * lock covers the pool and the roster for the blink of a list update
 * and is never held across a device command or a sleep.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/container/ring_queue.hpp"
#include "cinux/container/slot_table.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {

/**
 * @brief   What one parked request asks the device to do.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
enum class RequestKind : uint8_t {
    kRead,   ///< Move blocks from the device into memory.
    kWrite,  ///< Move blocks from memory onto the device.
    kFlush,  ///< Order earlier writes before any later one.
};

/**
 * @brief   One caller's parked request, woken by whoever drove it.
 * @note    Lives in a stable slot on purpose: the completion and the
 *          sleepers' patience outlive the enqueue call, and their
 *          addresses must not move.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct BlockRequest {
    Lba              first{};         ///< First block of the request.
    BlockSpan        span{};          ///< How many blocks it spans.
    void*            data = nullptr;  ///< Caller's buffer; null for a flush.
    RequestKind      kind = RequestKind::kRead;
    proc::Completion done;  ///< The driver's verdict, finished once per request.
};

/**
 * @brief   Serializes many callers onto one seated block device.
 * @note    Instance class on purpose: the host test world seats a mock
 *          disk and drives it single-handed, the kernel seats the AHCI
 *          drive with tasks on both ends. Task contexts only: the
 *          waiter sleeps on the request's completion, which has no
 *          answer outside the scheduler.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
class BlockQueue {
public:
    /**
     * @brief         Seats the one device this queue serves.
     *
     * @param[in,out] device  Device instance; its lifetime must cover
     *                        every later call.
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    template <BlockDevice Dev>
    explicit BlockQueue(Dev& device) {
        device_.adopt(device);
    }

    /**
     * @brief         Reads blocks through the queue.
     *
     * @param[in]     first        First block to read.
     * @param[in]     span         Blocks to read.
     * @param[out]    destination  Memory the blocks land in.
     * @return        True when the device moved every block, false on
     *                      a device refusal or a full request pool.
     * @warning       Task contexts only; calling before the scheduler
     *                      runs parks forever.
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool read(Lba first, BlockSpan span, uint8_t* destination);

    /**
     * @brief         Writes blocks through the queue.
     *
     * @param[in]     first   First block to write.
     * @param[in]     span    Blocks to write.
     * @param[in]     source  Memory the blocks leave from.
     * @return        True when the device took every block, false on
     *                      a device refusal or a full request pool.
     * @warning       Task contexts only, ditto.
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool write(Lba first, BlockSpan span, const uint8_t* source);

    /**
     * @brief         Orders seated writes before any later one.
     *
     * @return        True when the device flushed.
     * @note          Goes through the same roster, so a flush never
     *                      overtakes a parked write.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool flush();

private:
    /**
     * @brief         Parks one request and hands back its slot.
     *
     * @param[in]     first  First block of the request.
     * @param[in]     span   Span of the request.
     * @param[in]     data   Caller's buffer.
     * @param[in]     kind   What the request asks.
     * @return        Slot index, or kBlockQueueDepth when the pool is
     *                      full.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    unsigned long enqueue(Lba first, BlockSpan span, void* data, RequestKind kind);

    /**
     * @brief         Sleeps until the request in slot finished, then
     *                      reclaims the slot.
     *
     * @param[in]     slot  Slot from enqueue.
     * @return        The device verdict for this request.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool await(unsigned long slot);

    /**
     * @brief         Drives every parked request to completion.
     * @note          The duty itself: called with the lock free, holds
     *                it only around roster updates, never across a
     *                device command.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void drive();

    BlockPort device_;

    proc::Spinlock lock_;

    base::container::SlotTable<BlockRequest, kBlockQueueDepth>     pool_;
    base::container::RingQueue<unsigned int, kBlockQueueDepth + 1> pending_;
    bool                                                           duty_taken_ = false;
};

}  // namespace cinux::driver
