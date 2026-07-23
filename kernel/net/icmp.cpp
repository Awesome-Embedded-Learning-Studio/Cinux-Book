/**
 * @file kernel/net/icmp.cpp
 * @brief IcmpModule -- echo request -> reply, reply -> record.
 *
 * On an echo request: copy the whole ICMP message, flip type to 0 (reply),
 * zero + recompute the ICMP checksum (over header + data), and emit via
 * Ipv4Module::send to the request's source IP.  On an echo reply: record id/seq
 * + count so a ping originator can observe the round-trip.  Zero kprintf.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/icmp.hpp"

#include <cinux/checksum.hpp>

#include "kernel/net/net_stack.hpp"  // NetStack (full def for ipv4.send path)

namespace cinux::net {

namespace {
/// Max ICMP message we will mirror (fits one Ethernet frame minus L2+IPv4).
constexpr uint32_t kMaxIcmp = 1518 - 14 - 20;
}  // namespace

void IcmpModule::handle(const Ipv4Header& ip, FrameView payload, NetDevice& dev, Ipv4Module& ipv4,
                        NetStack& stack) {
    if (payload.size() < sizeof(IcmpHeader)) {
        return;  // short / not an echo message
    }
    IcmpHeader hdr;
    parse_icmp(payload.data(), hdr);

    if (hdr.type == kIcmpEchoRequest) {
        const uint32_t n = payload.size();
        if (n > kMaxIcmp) {
            return;  // too big to mirror (sanity)
        }
        uint8_t buf[kMaxIcmp];
        for (uint32_t i = 0; i < n; ++i) {
            buf[i] = payload.data()[i];  // copy header + echo data verbatim
        }
        buf[0]            = kIcmpEchoReply;  // type 0
        buf[1]            = 0;               // code 0
        buf[2]            = 0;
        buf[3]            = 0;  // zero checksum before recompute
        const uint16_t cs = cinux::lib::internet_checksum(buf, n);
        buf[2]            = static_cast<uint8_t>(cs >> 8);
        buf[3]            = static_cast<uint8_t>(cs & 0xFF);
        // Reply to the request's source; Ipv4Module sources our local address.
        (void)ipv4.send(dev, ip.src, kIpProtoIcmp, buf, n, stack);
    } else if (hdr.type == kIcmpEchoReply) {
        ++reply_count_;
        last_id_  = hdr.id;
        last_seq_ = hdr.seq;
    }
}

cinux::lib::ErrorOr<void> IcmpModule::send_echo_request(NetDevice& dev, Ipv4Addr dst, uint16_t id,
                                                        uint16_t seq, Ipv4Module& ipv4,
                                                        NetStack& stack) {
    uint8_t    buf[sizeof(IcmpHeader)];
    IcmpHeader h{};
    h.type = kIcmpEchoRequest;
    h.code = 0;
    h.id   = id;
    h.seq  = seq;
    build_icmp_header(h, buf);  // checksum field zeroed in h
    const uint16_t cs = cinux::lib::internet_checksum(buf, sizeof(buf));
    buf[2]            = static_cast<uint8_t>(cs >> 8);
    buf[3]            = static_cast<uint8_t>(cs & 0xFF);
    return ipv4.send(dev, dst, kIpProtoIcmp, buf, sizeof(buf), stack);
}

}  // namespace cinux::net
