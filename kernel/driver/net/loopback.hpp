/**
 * @file    loopback.hpp
 * @brief   The loopback: the network family's first resident, all
 *          software.
 *
 * A network device with no wire. send_l3 queues the payload as given,
 * poll_frame hands it back in the same order, and the honest answers
 * are the teaching point: no hardware address, no Ethernet header, and
 * a queue that refuses when a sender outruns a reader. The frame the
 * sender pushed is the frame the reader pulls, byte for byte, so a
 * protocol stack can prove itself here deterministically before any
 * real card arrives.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>

#include "cinux/container/ring_queue.hpp"
#include "cinux/singleton.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"

namespace cinux::driver::net {

/**
 * @brief   The one loopback device, satisfying NetworkDevice.
 * @note    The queue is bring-up state: init() empties it once, sends
 *          and polls own it afterwards. Frames travel through the same
 *          bounded ring every producer-consumer pair in the kernel
 *          uses; the ring answers full and empty honestly rather than
 *          dropping silently.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
class Loopback : public cinux::base::Singleton<Loopback> {
    friend class cinux::base::Singleton<Loopback>;

public:
    /**
     * @brief         Empties the frame queue.
     * @return        None
     * @note          Bring-up wiring; also how a test returns the one
     *                instance to a clean slate.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    void init();

    /**
     * @brief   Whether this device owns a hardware address.
     * @return  False, the honest answer for a wire that isn't there.
     * @since   0.6.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] bool has_mac() const;

    /**
     * @brief   This device's hardware address.
     * @return  Zeros; there is no address to read.
     * @since   0.6.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] MacAddress mac() const;

    /**
     * @brief   Whether this device's wire carries an Ethernet header.
     * @return  False; payloads pass through exactly as sent.
     * @since   0.6.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] bool has_ethernet_header() const;

    /**
     * @brief   The largest frame this device moves.
     * @return  The family maximum, inherited from the config word.
     * @since   0.6.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] unsigned long max_frame() const;

    /**
     * @brief         Hands the oldest queued frame out.
     * @param[out]    into   The frame filled with the next payload.
     * @return        True with a frame; false when the queue is empty.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool poll_frame(RxFrame& into);

    /**
     * @brief         Queues one layer-three payload.
     * @param[in]     destination  Ignored; there is no wire to frame for.
     * @param[in]     type         Ignored alongside the destination.
     * @param[in]     payload      The payload bytes to queue.
     * @param[in]     length       How many payload bytes, one upward.
     * @return        True when the payload queued; false for an empty or
     *                oversized payload, or a full queue.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool send_l3(const MacAddress& destination, EtherType type,
                               const std::uint8_t* payload, unsigned long length);

private:
    Loopback() = default;

    cinux::base::container::RingQueue<RxFrame, kLoopbackQueueFrames + 1> queue_ = {};
};

}  // namespace cinux::driver::net
