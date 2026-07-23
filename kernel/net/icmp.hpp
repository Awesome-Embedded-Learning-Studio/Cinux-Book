/**
 * @file kernel/net/icmp.hpp
 * @brief ICMP wire layout + IcmpModule (echo request -> reply, reply -> record).
 *
 * An L4Handler registered into Ipv4Module's proto table (ICMP is IP proto 1,
 * NOT a separate ethertype -- so IcmpModule is NOT a ProtocolHandler; Ipv4Module
 * dispatches it via the inner L4 table, not the ethertype table).  On an echo
 * request it builds a reply and hands it to Ipv4Module::send; on an echo reply
 * it records id/seq so a ping originator can observe the round-trip.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/ipv4.hpp"  // Ipv4Header, Ipv4Module

namespace cinux::net {

constexpr uint8_t kIcmpEchoRequest = 8;
constexpr uint8_t kIcmpEchoReply   = 0;

/// @brief ICMP header, HOST-order parsed view.  8 bytes (echo).
struct IcmpHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;  ///< host order (over the whole ICMP message)
    uint16_t id;        ///< host order (echo identifier)
    uint16_t seq;       ///< host order (echo sequence)
};
static_assert(sizeof(IcmpHeader) == 8, "ICMP echo header is 8 bytes");

inline void parse_icmp(const uint8_t* p, IcmpHeader& out) {
    out.type     = p[0];
    out.code     = p[1];
    out.checksum = (static_cast<uint16_t>(p[2]) << 8) | p[3];
    out.id       = (static_cast<uint16_t>(p[4]) << 8) | p[5];
    out.seq      = (static_cast<uint16_t>(p[6]) << 8) | p[7];
}

inline void build_icmp_header(const IcmpHeader& in, uint8_t* p) {
    p[0] = in.type;
    p[1] = in.code;
    p[2] = static_cast<uint8_t>(in.checksum >> 8);
    p[3] = static_cast<uint8_t>(in.checksum & 0xFF);
    p[4] = static_cast<uint8_t>(in.id >> 8);
    p[5] = static_cast<uint8_t>(in.id & 0xFF);
    p[6] = static_cast<uint8_t>(in.seq >> 8);
    p[7] = static_cast<uint8_t>(in.seq & 0xFF);
}

class IcmpModule : public L4Handler {
public:
    /// @brief Handle an inbound ICMP message (L4Handler, dispatched for proto==1).
    ///        Echo request -> echo reply (via ipv4.send); echo reply -> record.
    void handle(const Ipv4Header& ip, FrameView payload, NetDevice& dev, Ipv4Module& ipv4,
                NetStack& stack) override;

    /// @brief Send an ICMP echo request (the ping originator's TX).  src comes
    ///        from the device's InDevice (Ipv4Module::send sources it).
    cinux::lib::ErrorOr<void> send_echo_request(NetDevice& dev, Ipv4Addr dst, uint16_t id,
                                                uint16_t seq, Ipv4Module& ipv4, NetStack& stack);

    /// @brief Echo-reply observation (a ping originator reads these after poll).
    uint32_t reply_count() const { return reply_count_; }
    uint16_t last_reply_id() const { return last_id_; }
    uint16_t last_reply_seq() const { return last_seq_; }
    void     reset() {
        reply_count_ = 0;
        last_id_     = 0;
        last_seq_    = 0;
    }

private:
    uint32_t reply_count_ = 0;  ///< echo replies observed
    uint16_t last_id_     = 0;
    uint16_t last_seq_    = 0;
};

}  // namespace cinux::net
