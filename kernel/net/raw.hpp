/**
 * @file    raw.hpp
 * @brief   The raw ICMP module: subscriptions, whole-datagram delivery.
 *
 * A raw socket wants the datagram as it travelled, IPv4 header and all,
 * because a ping counts its round trips from the wire's own words. One
 * registry keeps the subscribers; delivery copies the borrowed bytes to
 * each under the registry lock, and removal waits for any in-flight
 * delivery to finish before the subscriber's ring dies. Sends skip the
 * UDP word book entirely: the caller's message leaves as-is inside an
 * IPv4 envelope the module wraps around it.
 *
 * @author  Charliechen114514
 * @date    2026-10-24
 * @version 0.1
 * @since   0.7.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/net/ipv4.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::net {

/**
 * @brief   ICMP raw subscriptions; removal waits for borrowed delivery.
 * @note    Delivery runs under the registry gate, so a receive callback
 *          must copy fast and must not sleep or re-enter the registry.
 * @since   0.7.0
 * @ingroup kernel_net
 */
class RawModule {
public:
    /// One subscriber's entry point: the whole datagram plus its header.
    using Receive = void (*)(void*, const unsigned char*, size_t, const Ipv4View&);

    /**
     * @brief         Names the module this registry sends through.
     *
     * @param[in,out] ipv4   The IPv4 module whose send path raw traffic rides.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    constexpr explicit RawModule(Ipv4Module& ipv4) : ipv4_(ipv4) {}

    /**
     * @brief         Registers one ICMP subscriber.
     *
     * @param[in]     context   The subscriber's own context, opaque here.
     * @param[in]     receive   The subscriber's entry point.
     * @return        True when seated; false when the registry is full or
     *                the pair is incomplete.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    bool bind(void* context, Receive receive);

    /**
     * @brief         Removes one subscriber before its receive ring dies.
     *
     * @param[in]     context   The context the subscriber registered with.
     * @return        None
     * @note          Waits for any borrowed delivery still running, so the
     *                ring may be destroyed on return.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void unbind(void* context);

    /**
     * @brief         Copies a borrowed datagram to each subscriber.
     *
     * @param[in]     bytes   First byte of the whole datagram, header included.
     * @param[in]     length  Bytes the datagram holds.
     * @param[in]     header  The parsed IPv4 header it travelled in.
     * @return        None
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void handle(const unsigned char* bytes, size_t length, const Ipv4View& header);

    /**
     * @brief         Wraps a user ICMP message in an IPv4 datagram.
     *
     * @param[in]     destination  Address the envelope names.
     * @param[in]     bytes        First byte of the caller's message.
     * @param[in]     length       Message bytes.
     * @return        True when built and sent; false for an oversized
     *                message or an unassigned send path.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    bool send(uint32_t destination, const unsigned char* bytes, size_t length);

private:
    /**
     * @brief   One seated subscriber.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    struct Listener {
        void*   context = nullptr;
        Receive receive = nullptr;
    };

    static constexpr unsigned long kListeners = 16;
    Ipv4Module&                    ipv4_;
    proc::Spinlock                 gate_;
    Listener                       listeners_[kListeners]{};
};

}  // namespace cinux::net
