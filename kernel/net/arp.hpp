/**
 * @file    arp.hpp
 * @brief   Address resolution as pure bookkeeping.
 *
 * The cache learns from every ARP packet it observes — replies carry
 * the answer, requests carry the asker's own pair — and lookup answers
 * from what was learned. A miss is not an error to paper over: the
 * caller sends a request this turn and asks again the next, which is
 * the asynchronous resolve the station teaches. Building a request is
 * a pure function into a caller-owned buffer; nothing here sends,
 * receives, or knows a device.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"

namespace cinux::net {

/**
 * @brief   The learned address pairs, pure bookkeeping.
 * @since   0.6.0
 * @ingroup kernel_net
 */
class ArpCache {
public:
    /**
     * @brief         Returns the cache to empty.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void init();

    /**
     * @brief         Looks one protocol address up.
     *
     * @param[in]     protocol_address  The IPv4 address to resolve.
     * @return        The learned hardware address, or nullptr when
     *                nothing was learned yet.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] const driver::net::MacAddress* lookup(uint32_t protocol_address) const;

    /**
     * @brief         Learns from one observed ARP packet.
     *
     * @param[in]     packet  First byte of the packet.
     * @param[in]     length  Bytes the packet holds.
     * @return        True when the packet parsed and its sender pair
     *                was learned; false when the word book refused it.
     * @note          Requests and replies both teach: each carries the
     *                sender's own pair. The target pair of a request is
     *                unknown by design and never learned from.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    bool observe(const unsigned char* packet, size_t length);

    /**
     * @brief         Writes one ARP request for a caller to send.
     *
     * @param[out]    packet         Buffer of kArpPacketBytes bytes.
     * @param[in]     sender_hardware  The sender's hardware address, the kernel's own.
     * @param[in]     sender_protocol  The sender's IPv4 address, the kernel's own.
     * @param[in]     target_protocol  The address being resolved.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    static void build_request(unsigned char* packet, const driver::net::MacAddress& sender_hardware,
                              uint32_t sender_protocol, uint32_t target_protocol);

private:
    struct Entry {
        uint32_t                protocol = 0;
        driver::net::MacAddress hardware = {};
        bool                    used     = false;
    };

    Entry         entries_[kArpCacheEntries] = {};
    unsigned long next_                      = 0;
};

}  // namespace cinux::net
