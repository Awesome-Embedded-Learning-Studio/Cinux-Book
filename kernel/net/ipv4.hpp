/**
 * @file kernel/net/ipv4.hpp
 * @brief IPv4 wire layout + Ipv4Module (ethertype 0x0800 handler) + TX.
 *
 * HOST-order parsed header (see net_types.hpp byte-order policy).  Ipv4Module
 * validates an inbound packet (version / IHL / header checksum via
 * internet_checksum) and, for proto==ICMP, hands the L4 payload to a composed
 * IcmpModule (a fixed 3-node graph for ping -- NOT an inner proto table; a
 * TODO documents that extension for future TCP/UDP).  send() builds an IPv4
 * header + emits on a device (resolving next-hop via ARP for Ethernet,
 * skipping L2 for loopback).
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/net_types.hpp"
#include "kernel/net/protocol_handler.hpp"

namespace cinux::net {

class ArpModule;   // forward -- next-hop resolver (ipv4.cpp includes arp.hpp)
class IcmpModule;  // forward -- composed L4 handler

constexpr uint8_t kIpProtoIcmp    = 1;
constexpr uint8_t kIpv4HdrWords   = 5;  // 20-byte fixed header (IHL)
constexpr uint8_t kIpv4TtlDefault = 64;

/// @brief IPv4 header, HOST-order parsed view.  20 bytes for IHL=5.
struct Ipv4Header {
    uint8_t  ver_ihl;  ///< version (hi) | IHL (lo)
    uint8_t  dscp_ecn;
    uint16_t total_len;   ///< host order (header + data)
    uint16_t id;          ///< host order
    uint16_t flags_frag;  ///< host order
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;  ///< host order
    Ipv4Addr src;
    Ipv4Addr dst;
};
static_assert(sizeof(Ipv4Header) == 20, "IPv4 fixed header is 20 bytes");

inline uint8_t ipv4_version(const Ipv4Header& h) {
    return h.ver_ihl >> 4;
}
inline uint8_t ipv4_ihl(const Ipv4Header& h) {
    return h.ver_ihl & 0x0F;
}

/// @brief Parse 20 wire bytes into an Ipv4Header (host order).
inline void parse_ipv4(const uint8_t* p, Ipv4Header& out) {
    out.ver_ihl    = p[0];
    out.dscp_ecn   = p[1];
    out.total_len  = (static_cast<uint16_t>(p[2]) << 8) | p[3];
    out.id         = (static_cast<uint16_t>(p[4]) << 8) | p[5];
    out.flags_frag = (static_cast<uint16_t>(p[6]) << 8) | p[7];
    out.ttl        = p[8];
    out.proto      = p[9];
    out.checksum   = (static_cast<uint16_t>(p[10]) << 8) | p[11];
    for (int i = 0; i < 4; ++i) {
        out.src.oct[i] = p[12 + i];
        out.dst.oct[i] = p[16 + i];
    }
}

/// @brief Serialise an Ipv4Header into 20 wire bytes (big-endian multi-byte).
inline void build_ipv4_header(const Ipv4Header& in, uint8_t* p) {
    p[0]  = in.ver_ihl;
    p[1]  = in.dscp_ecn;
    p[2]  = static_cast<uint8_t>(in.total_len >> 8);
    p[3]  = static_cast<uint8_t>(in.total_len & 0xFF);
    p[4]  = static_cast<uint8_t>(in.id >> 8);
    p[5]  = static_cast<uint8_t>(in.id & 0xFF);
    p[6]  = static_cast<uint8_t>(in.flags_frag >> 8);
    p[7]  = static_cast<uint8_t>(in.flags_frag & 0xFF);
    p[8]  = in.ttl;
    p[9]  = in.proto;
    p[10] = static_cast<uint8_t>(in.checksum >> 8);
    p[11] = static_cast<uint8_t>(in.checksum & 0xFF);
    for (int i = 0; i < 4; ++i) {
        p[12 + i] = in.src.oct[i];
        p[16 + i] = in.dst.oct[i];
    }
}

class Ipv4Module : public ProtocolHandler {
public:
    /// @param icmp  the composed ICMP handler (echo req -> reply, reply -> record).
    /// @param arp   the next-hop resolver (may be null when no L2 / no ARP).
    Ipv4Module(IcmpModule& icmp, ArpModule* arp) : icmp_(icmp), arp_(arp) {}

    /// @brief Validate + dispatch an inbound IPv4 packet (proto==ICMP -> Icmp).
    void on_frame(const L2Info& l2, FrameView payload, NetDevice& dev, NetStack& stack) override;

    /// @brief Build an IPv4 header (src from the device's InDevice) + send on
    ///        @p dev.  Loopback skips L2; Ethernet resolves @p dst via ARP
    ///        (async -- if unresolved, an ARP request is sent and the IP packet
    ///        is deferred to the next poll; not an error).
    cinux::lib::ErrorOr<void> send(NetDevice& dev, Ipv4Addr dst, uint8_t proto, const uint8_t* l4,
                                   uint32_t len, NetStack& stack);

private:
    IcmpModule& icmp_;
    ArpModule*  arp_;  ///< nullable (null when no L2 / ARP unavailable)
    uint16_t    next_id_ = 0;
};

}  // namespace cinux::net
