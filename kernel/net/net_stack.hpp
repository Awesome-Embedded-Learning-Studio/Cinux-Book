/**
 * @file    net_stack.hpp
 * @brief   The protocol stack: ports, protocol seats, and a budgeted
 *          poll.
 *
 * The stack owns no protocol. Devices arrive as ports from the device
 * family, protocols arrive as seats registered by ethertype, and poll
 * moves frames from the former to the latter under a frame budget that
 * also guards re-entry — a handler that sends lands its frame in a
 * queue the same drain picks up, and the budget is the loop's leash.
 * A port without an Ethernet header — the loopback — delivers its bytes
 * straight to the IPv4 seat: there is no wire to frame for. Refusals
 * are counted, not printed: the counters are the negative tests'
 * handles.
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

#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"

namespace cinux::net {

/**
 * @brief   What a layer-three protocol offers, checked at compile time.
 * @tparam  Handler   The protocol type being checked.
 * @note    handle receives the payload after any Ethernet header, or
 *          the whole frame when the port carries none.
 * @since   0.6.0
 * @ingroup kernel_net
 */
template <typename Handler>
concept L3Handler = requires(Handler& handler, const unsigned char* bytes, size_t length) {
    { handler.handle(bytes, length) } -> std::same_as<void>;
};

/**
 * @brief   The erased protocol face: one entry point.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct ProtocolOps {
    /// Hand one payload to the seated protocol.
    void (*handle)(void* self, const unsigned char* bytes, size_t length);
};

/**
 * @brief   One protocol behind the face, ready to call.
 * @note    adopt() erases exactly once per protocol; an unadopted seat
 *          refuses every call.
 * @since   0.6.0
 * @ingroup kernel_net
 */
class ProtocolSeat {
public:
    ProtocolSeat() = default;

    /**
     * @brief         Seats one concept-checked protocol.
     *
     * @param[in,out] protocol  The protocol instance taking the seat.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    template <L3Handler Handler>
    void adopt(Handler& protocol) {
        static constexpr ProtocolOps kOps = {
            [](void* self, const unsigned char* bytes, size_t length) {
                static_cast<Handler*>(self)->handle(bytes, length);
            },
        };
        ops_  = &kOps;
        self_ = &protocol;
    }

    /**
     * @brief         Hands one payload to the seated protocol.
     *
     * @param[in]     bytes   First payload byte.
     * @param[in]     length  Payload length.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void handle(const unsigned char* bytes, size_t length) const {
        if (ops_ != nullptr) {
            ops_->handle(self_, bytes, length);
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
    const ProtocolOps* ops_  = nullptr;
    void*              self_ = nullptr;
};

/**
 * @brief   What one poll delivered and what it refused.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct NetStackStats {
    /// Frames handed to a seated protocol.
    unsigned long delivered        = 0;
    /// Frames naming an ethertype nobody seated.
    unsigned long unknown_protocol = 0;
    /// Frames whose Ethernet header the word book refused.
    unsigned long refused_ethernet = 0;
};

/**
 * @brief   Ports plus protocol seats plus the budgeted drain.
 * @note    A plain instance, not a singleton: the wiring owns it, the
 *          tests build their own, and nothing here reaches for hardware.
 * @since   0.6.0
 * @ingroup kernel_net
 */
class NetStack {
public:
    /**
     * @brief         Returns the counters to zero.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void init();

    /**
     * @brief         Attaches one device port.
     *
     * @param[in,out] port  The port to drain on every poll.
     * @return        True when attached; false when the table is full.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    bool attach_port(driver::net::NetPort& port);

    /**
     * @brief         Registers one protocol under an ethertype.
     *
     * @param[in]     ethertype  The protocol word the frame carries.
     * @param[in,out] seat       The protocol taking the seat.
     * @return        True when seated; false when the table is full or
     *                the word is already taken.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    bool seat_protocol(uint16_t ethertype, ProtocolSeat seat);

    /**
     * @brief         Drains every port under one frame budget.
     *
     * @param[in]     budget  How many frames this call may deliver.
     * @return        None
     * @note          The budget spans all ports and is the re-entry
     *                leash: a handler whose send lands a frame in a
     *                queue sees it drained only while budget remains.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void poll(unsigned long budget);

    /**
     * @brief         The delivery and refusal counters so far.
     * @return        A copy of the current statistics.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] const NetStackStats& stats() const;

private:
    void dispatch(const driver::net::NetPort& port, const driver::net::RxFrame& frame);

    driver::net::NetPort* ports_[kMaxPorts] = {};
    unsigned long         port_count_       = 0;

    struct SeatedProtocol {
        uint16_t     ethertype = 0;
        ProtocolSeat seat;
    };
    SeatedProtocol protocols_[kMaxProtocolSeats] = {};
    unsigned long  protocol_count_               = 0;

    NetStackStats stats_ = {};
};

}  // namespace cinux::net
