/**
 * @file    ipv4.hpp
 * @brief   The IPv4 module: verify, dispatch, and build.
 *
 * Incoming datagrams are judged twice — the word book parses them, then
 * the one's-complement sum over the header must fold to zero — and every
 * refusal lands in its own counter, because the negative paths are the
 * station's teaching content. Payloads that survive reach the layer-four
 * seat registered under the header's protocol word, carrying the parsed
 * header so a reply can answer its asker. Outgoing datagrams are built
 * whole here: fixed five-word header, correct checksum, then handed to
 * a sink the wiring aims at whichever port is right.
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

#include <concepts>

#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

/**
 * @brief   One datagram on its way out, all its facts together.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct OutboundDatagram {
    /// Address the header names.
    uint32_t             destination = 0;
    /// Protocol word the header carries.
    unsigned char        protocol    = 0;
    /// First payload byte.
    const unsigned char* payload     = nullptr;
    /// Payload bytes, zero allowed.
    size_t               length      = 0;
};

/**
 * @brief   Where a finished datagram goes, injected by the wiring.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct DatagramSink {
    /// The wiring's context, opaque here.
    void* context                                                 = nullptr;
    /// Hands one datagram to the port path the wiring chose.
    void (*send)(void* context, const OutboundDatagram& datagram) = nullptr;

    /**
     * @brief         Whether the sink can deliver.
     * @return        True when both parts are set.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool assigned() const { return context != nullptr && send != nullptr; }
};

/**
 * @brief   What a layer-four protocol offers, checked at compile time.
 * @tparam  Handler   The protocol type being checked.
 * @note    handle receives the payload plus the header it travelled in.
 * @since   0.6.0
 * @ingroup kernel_net
 */
template <typename Handler>
concept L4Handler =
    requires(Handler& handler, const unsigned char* bytes, size_t length, const Ipv4View& header) {
        { handler.handle(bytes, length, header) } -> std::same_as<void>;
    };

/**
 * @brief   The erased layer-four face: one entry point.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct L4Ops {
    /// Hand one payload and its header to the seated protocol.
    void (*handle)(void* self, const unsigned char* bytes, size_t length, const Ipv4View& header);
};

/**
 * @brief   One layer-four protocol behind the face, ready to call.
 * @since   0.6.0
 * @ingroup kernel_net
 */
class L4Seat {
public:
    L4Seat() = default;

    /**
     * @brief         Seats one concept-checked protocol.
     *
     * @param[in,out] protocol  The protocol instance taking the seat.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    template <L4Handler Handler>
    void adopt(Handler& protocol) {
        static constexpr L4Ops kOps = {
            [](void* self, const unsigned char* bytes, size_t length, const Ipv4View& header) {
                static_cast<Handler*>(self)->handle(bytes, length, header);
            },
        };
        ops_  = &kOps;
        self_ = &protocol;
    }

    /**
     * @brief         Hands one payload and its header to the protocol.
     *
     * @param[in]     bytes   First payload byte.
     * @param[in]     length  Payload length.
     * @param[in]     header  The IPv4 header this payload travelled in.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void handle(const unsigned char* bytes, size_t length, const Ipv4View& header) const {
        if (ops_ != nullptr) {
            ops_->handle(self_, bytes, length, header);
        }
    }

    /**
     * @brief         Whether a protocol sits in this seat.
     * @return        True once adopt() has run.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool seated() const { return ops_ != nullptr; }

private:
    const L4Ops* ops_  = nullptr;
    void*        self_ = nullptr;
};

/**
 * @brief   Every refusal reason, counted separately.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct Ipv4Stats {
    /// Datagrams handed to a seated protocol.
    unsigned long delivered            = 0;
    /// Datagrams the word book refused as too short.
    unsigned long refused_short        = 0;
    /// Datagrams whose version nibble was not four.
    unsigned long refused_version      = 0;
    /// Datagrams whose header promised fewer than five words.
    unsigned long refused_header_words = 0;
    /// Datagrams whose total fell short of their own header.
    unsigned long refused_total_length = 0;
    /// Datagrams whose header checksum did not fold to zero.
    unsigned long refused_checksum     = 0;
    /// Datagrams naming a protocol nobody seated.
    unsigned long unknown_protocol     = 0;
    /// Datagrams this module built and handed to the sink.
    unsigned long sent                 = 0;
};

/**
 * @brief   The IPv4 module, a plain instance the wiring owns.
 * @since   0.6.0
 * @ingroup kernel_net
 */
class Ipv4Module {
public:
    /**
     * @brief         Aims the module at its sink and address.
     *
     * @param[in]     sink          Where built datagrams go.
     * @param[in]     own_address   Source address outgoing headers carry.
     * @return        None
     * @since         0.6.0
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
     *                runs first, a wire joins later, and counters and
     *                seats survive the aim.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void aim(const DatagramSink& sink, uint32_t own_address);

    /**
     * @brief         Registers one layer-four protocol.
     *
     * @param[in]     protocol  The protocol word the header carries.
     * @param[in,out] seat      The protocol taking the seat.
     * @return        True when seated; false when the table is full or
     *                the word is already taken.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    bool seat_protocol(unsigned char protocol, L4Seat seat);

    /**
     * @brief         Judges and dispatches one incoming datagram.
     *
     * @param[in]     bytes   First datagram byte.
     * @param[in]     length  Bytes the datagram holds.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void handle(const unsigned char* bytes, size_t length);

    /**
     * @brief         Builds and sends one datagram.
     *
     * @param[in]     datagram  The destination, protocol, and payload.
     * @return        True when built and handed to the sink; false for
     *                an oversized payload or an unassigned sink.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    bool send(const OutboundDatagram& datagram);

    /**
     * @brief         The delivery and refusal counters so far.
     * @return        A reference to the current statistics.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] const Ipv4Stats& stats() const;

    /** @brief Observes validated IPv4 datagrams before protocol dispatch; configured at boot. */
    void observe(void* context,
                 void (*receive)(void*, const unsigned char*, size_t, const Ipv4View&));


private:
    struct SeatedProtocol {
        unsigned char protocol = 0;
        L4Seat        seat;
    };

    DatagramSink   sink_                                                    = {};
    uint32_t       own_address_                                             = 0;
    SeatedProtocol protocols_[kMaxL4Seats]                                  = {};
    unsigned long  protocol_count_                                          = 0;
    Ipv4Stats      stats_                                                   = {};
    void*          observer_context_                                        = nullptr;
    void (*observer_)(void*, const unsigned char*, size_t, const Ipv4View&) = nullptr;
};

}  // namespace cinux::net
