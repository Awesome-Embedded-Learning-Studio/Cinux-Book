/**
 * @file    udp.hpp
 * @brief   The UDP module: ports, checks, and a header builder.
 *
 * One module faces both directions. Incoming datagrams are judged three
 * times — the word book parses the header, the announced length must sit
 * inside the bytes that arrived, and when a checksum was sent at all the
 * one's-complement sum over the pseudo header plus the datagram must
 * fold to zero — and every refusal lands in its own counter. What
 * survives reaches the listener seated under the destination port; a
 * datagram nobody claims dies counted, the family's discipline.
 * Outgoing datagrams are built whole here: fixed eight-byte header,
 * checksum computed over the pseudo header with the zero rule honored —
 * a computed zero is sent as 0xFFFF, because an honest zero on the wire
 * means "no checksum was computed" — then handed to the sink the wiring
 * points at the IPv4 module. The pseudo header needs the source address
 * the IPv4 header carries, so this module holds its own copy of the
 * wiring's address and re-aims alongside it when a wire joins.
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

#include "cinux/spin_mutex.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

/**
 * @brief   One datagram handed to a listener, its return address with it.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct UdpDelivery {
    /// Address the datagram travelled from.
    uint32_t             sender      = 0;
    /// Port the datagram travelled from.
    uint16_t             sender_port = 0;
    /// First payload byte.
    const unsigned char* bytes       = nullptr;
    /// Payload bytes.
    size_t               length      = 0;
};

/**
 * @brief   What a bound port offers: one entry point, the wiring's context.
 * @since   0.7.0
 * @ingroup kernel_net
 */
using UdpDeliver = void (*)(void* context, const UdpDelivery& delivery);

/**
 * @brief   The delivery and refusal counters so far.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct UdpStats {
    /// Datagrams handed to a seated listener.
    unsigned long delivered        = 0;
    /// Datagrams the word book refused as too short.
    unsigned long refused_short    = 0;
    /// Datagrams whose announced length disagreed with the bytes held.
    unsigned long refused_length   = 0;
    /// Datagrams whose checksum did not fold to zero.
    unsigned long refused_checksum = 0;
    /// Datagrams naming a port nobody bound.
    unsigned long unclaimed        = 0;
    /// Datagrams this module built and handed to the sink.
    unsigned long sent             = 0;
};

/**
 * @brief   The UDP module, a plain instance the wiring owns.
 * @note    A listener's bytes pointer lives only for the call: the
 *          receive buffer is reused by the next datagram, so a listener
 *          that wants the payload must copy it before returning. The
 *          station's own lesson, learned the hard way once.
 * @since   0.7.0
 * @ingroup kernel_net
 */
class UdpModule {
public:
    /**
     * @brief         Aims the module at its sink and source address.
     *
     * @param[in]     sink          Where built datagrams go.
     * @param[in]     own_address   Source address the pseudo header sums.
     * @return        None
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void init(const DatagramSink& sink, uint32_t own_address);

    /**
     * @brief         Re-aims the module without forgetting anything.
     *
     * @param[in]     sink          The new outgoing path.
     * @param[in]     own_address   The new source address.
     * @return        None
     * @note          For the boot wiring's reroute: the loopback world
     *                runs first, a wire joins later, and the port table
     *                and counters survive the aim.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void aim(const DatagramSink& sink, uint32_t own_address);

    /**
     * @brief         Judges one datagram and hands it to its listener.
     *
     * @param[in]     bytes   First datagram byte.
     * @param[in]     length  Bytes the datagram holds.
     * @param[in]     header  The IPv4 header it travelled in.
     * @return        None
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void handle(const unsigned char* bytes, size_t length, const Ipv4View& header);

    /**
     * @brief         Builds and sends one datagram.
     *
     * @param[in]     destination       Address the header names.
     * @param[in]     source_port       Port the header answers to.
     * @param[in]     destination_port  Port the header names.
     * @param[in]     payload           First payload byte.
     * @param[in]     length            Payload bytes, zero allowed.
     * @return        True when built and handed to the sink; false for
     *                an oversized payload or an unassigned sink.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    bool send(uint32_t destination, uint16_t source_port, uint16_t destination_port,
              const unsigned char* payload, size_t length);

    /**
     * @brief         Seats one listener under a port.
     *
     * @param[in]     port      The port datagrams must name.
     * @param[in]     context   The listener's own context, opaque here.
     * @param[in]     deliver   The listener's entry point.
     * @return        True when seated; false when the table is full or
     *                the port is already taken.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    bool bind(uint16_t port, void* context, UdpDeliver deliver);

    /**
     * @brief         Frees a port for the next listener.
     *
     * @param[in]     port   The port to release.
     * @return        True when a listener left; false when none sat there.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    bool unbind(uint16_t port);

    /**
     * @brief         The first free port in the ephemeral range.
     * @return        A port no listener holds, or zero when the range
     *                is exhausted.
     * @note          For callers that ask for a port without naming
     *                one; the seat itself is taken by bind.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] uint16_t ephemeral_port() const;

    /**
     * @brief         The delivery and refusal counters so far.
     * @return        A reference to the current statistics.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] const UdpStats& stats() const;

    /**
     * @brief         The source address outgoing headers carry.
     * @return        The address the wiring last aimed this module at.
     * @note          A socket bound to the any-address answers this
     *                        word when asked to name itself.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] uint32_t own_address() const;

private:
    struct PortSeat {
        uint16_t   port    = 0;
        void*      context = nullptr;
        UdpDeliver deliver = nullptr;
    };

    DatagramSink            sink_        = {};
    uint32_t                own_address_ = 0;
    mutable base::SpinMutex ports_gate_;
    PortSeat                ports_[kUdpPortSeats] = {};
    unsigned long           port_count_           = 0;
    unsigned long           ephemeral_cursor_     = kUdpEphemeralFirst;
    UdpStats                stats_                = {};
};

}  // namespace cinux::net
