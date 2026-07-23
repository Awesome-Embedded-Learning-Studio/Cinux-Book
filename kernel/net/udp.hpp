/**
 * @file kernel/net/udp.hpp
 * @brief UDP wire layout + UdpModule (L4Handler, IP proto 17) + port demux.
 *
 * A connectionless L4 protocol layered on Ipv4Module exactly like IcmpModule:
 * UdpModule is an L4Handler registered via Ipv4Module::add_l4(kIpProtoUdp, ...).
 * Inbound: validate the pseudo-header checksum, parse the ports, and hand the
 * payload to the UdpListener bound to the destination port (no listener -> silent
 * drop, like Linux).  Outbound: build the UDP header, compute the pseudo-header
 * checksum over [pseudo | header | payload], and emit via ipv4.send.
 *
 * Port multiplexing is a small fixed table (kMaxUdpPorts) -- the protocol-layer
 * primitive.  The socket API (bind/sendto/recvfrom) rides on top in F7-M6 and
 * grows the table then; this milestone delivers the protocol layer + tests only.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/ipv4.hpp"  // Ipv4Header, Ipv4Module, L4Handler, kIpProtoUdp
#include "kernel/net/net_types.hpp"

namespace cinux::net {

/// @brief UDP header, HOST-order parsed view.  8 bytes.
struct UdpHeader {
    uint16_t src_port;  ///< host order
    uint16_t dst_port;  ///< host order
    uint16_t length;    ///< host order (header + payload)
    uint16_t checksum;  ///< host order (pseudo-header based; 0 == "no checksum")
};
static_assert(sizeof(UdpHeader) == 8, "UDP header is 8 bytes");

/// @brief Parse 8 wire bytes into a UdpHeader (host order).
inline void parse_udp(const uint8_t* p, UdpHeader& out) {
    out.src_port = (static_cast<uint16_t>(p[0]) << 8) | p[1];
    out.dst_port = (static_cast<uint16_t>(p[2]) << 8) | p[3];
    out.length   = (static_cast<uint16_t>(p[4]) << 8) | p[5];
    out.checksum = (static_cast<uint16_t>(p[6]) << 8) | p[7];
}

/// @brief Serialise a UdpHeader into 8 wire bytes (big-endian multi-byte).
inline void build_udp_header(const UdpHeader& in, uint8_t* p) {
    p[0] = static_cast<uint8_t>(in.src_port >> 8);
    p[1] = static_cast<uint8_t>(in.src_port & 0xFF);
    p[2] = static_cast<uint8_t>(in.dst_port >> 8);
    p[3] = static_cast<uint8_t>(in.dst_port & 0xFF);
    p[4] = static_cast<uint8_t>(in.length >> 8);
    p[5] = static_cast<uint8_t>(in.length & 0xFF);
    p[6] = static_cast<uint8_t>(in.checksum >> 8);
    p[7] = static_cast<uint8_t>(in.checksum & 0xFF);
}

/// @brief Inbound UDP observer -- the consumer of datagrams for a bound port.
///
/// Registered with UdpModule::bind.  Like all net-stack callbacks, @p payload is
/// BORROWED: valid only for the duration of on_udp() -- a listener that needs
/// the bytes longer MUST copy them (the device recycles the buffer after
/// dispatch).
class UdpListener {
public:
    virtual ~UdpListener() = default;

    /// @brief A UDP datagram arrived addressed to this listener's port.
    /// @param ip       the IPv4 header (src = sender, for replies).
    /// @param src_port the sender's port.
    /// @param payload  the UDP payload (past the 8-byte header).  Borrowed.
    virtual void on_udp(const Ipv4Header& ip, uint16_t src_port, FrameView payload) = 0;
};

class UdpModule : public L4Handler {
public:
    static constexpr uint32_t kMaxUdpPorts = 16;  ///< protocol-layer table (socket layer grows it)

    /// @brief Bind @p listener to @p port.  Returns false if @p port is already
    ///        bound or the port table is full.  (Mirrors NetStack::attach's bool.)
    bool bind(uint16_t port, UdpListener& listener);

    /// @brief Release @p port.  No-op if not bound.
    void unbind(uint16_t port);

    /// @brief Send a UDP datagram.  The source IP comes from the device's
    ///        InDevice (same source Ipv4Module::send uses).  Builds the UDP
    ///        header + pseudo-header checksum, emits via ipv4.send (proto=17).
    cinux::lib::ErrorOr<void> send(NetDevice& dev, Ipv4Addr dst, uint16_t src_port,
                                   uint16_t dst_port, const uint8_t* data, uint32_t len,
                                   Ipv4Module& ipv4, NetStack& stack);

    /// @brief L4Handler: validate the pseudo-header checksum, parse the ports,
    ///        dispatch to the listener bound to the destination port.
    void handle(const Ipv4Header& ip, FrameView payload, NetDevice& dev, Ipv4Module& ipv4,
                NetStack& stack) override;

private:
    struct PortSlot {
        uint16_t     port = 0;
        UdpListener* l    = nullptr;
    };
    PortSlot ports_[kMaxUdpPorts]{};
};

}  // namespace cinux::net
