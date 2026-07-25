/**
 * @file test/unit/test_net_udp.cpp
 * @brief Host unit tests for UdpModule (L4Handler proto 17) + the Ipv4Module
 *        inner L4 table (the proto->handler dispatch UDP rides on).
 *
 * Strategy (底子优先): a no-L2 mock NetDevice lets a UDP send() be captured as
 * an IP packet, then fed straight back as a received frame -- a full send ->
 * IPv4 table dispatch -> UdpModule.handle -> listener round-trip on HOST, no
 * QEMU / SLIRP timing.  Covers: header round-trip, TX wire format, pseudo-header
 * checksum (compute on send, verify on handle), port demux (bind/unbind/dup),
 * checksum=0 ("no checksum"), and a corrupted-payload drop.
 *
 * Links the real udp.cpp + ipv4.cpp + icmp.cpp + net_stack.cpp + Cinux-Base
 * checksum.cpp (pure logic, host-linkable).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL

#include <cinux/checksum.hpp>
#include <cinux/expected.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_device.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/net/udp.hpp"
#include "test_framework.h"

using cinux::lib::internet_checksum;
using cinux::net::InDevice;
using cinux::net::Ipv4Addr;
using cinux::net::Ipv4Header;
using cinux::net::Ipv4Module;
using cinux::net::IcmpModule;
using cinux::net::kEtherTypeIpv4;
using cinux::net::kIpProtoUdp;
using cinux::net::NetDevice;
using cinux::net::NetStack;
using cinux::net::Packet;
using cinux::net::UdpHeader;
using cinux::net::UdpListener;
using cinux::net::UdpModule;

namespace {

Ipv4Addr ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return Ipv4Addr{{a, b, c, d}};
}

/// No-L2 (loopback-style) mock: yields queued RX frames and captures send_l3.
/// Copy device (sink==nullptr) -- frame bytes live in the vectors for the test's
/// lifetime, so handlers can borrow them during dispatch.
class NoL2Dev : public NetDevice {
public:
    std::vector<std::vector<uint8_t>> rx;
    std::vector<std::vector<uint8_t>> sent;
    size_t                            idx = 0;

    bool mac(cinux::net::EthAddr&) const override { return false; }
    bool has_ethernet_header() const override { return false; }
    bool poll_rx(Packet& out) override {
        if (idx >= rx.size()) {
            return false;
        }
        out.data = rx[idx].data();
        out.len  = static_cast<uint32_t>(rx[idx].size());
        out.sink = nullptr;
        ++idx;
        return true;
    }
    cinux::lib::ErrorOr<void> send_l3(const cinux::net::EthAddr&, uint16_t, const uint8_t* l3,
                                      uint32_t len) override {
        sent.emplace_back(l3, l3 + len);
        return {};
    }
};

/// Capture listener: records the last on_udp() invocation (copies payload).
struct Capture : UdpListener {
    uint32_t             calls         = 0;
    uint16_t             last_src_port = 0;
    Ipv4Addr             last_src{};
    std::vector<uint8_t> last_payload;
    void on_udp(const Ipv4Header& hdr, uint16_t src_port, cinux::net::FrameView payload) override {
        ++calls;
        last_src_port = src_port;
        last_src      = hdr.src;
        last_payload.assign(payload.data(), payload.data() + payload.size());
    }
};

/// Stack + modules wired for a no-L2 device at @p local.  ARP is null (a no-L2
/// device never resolves a next-hop); ICMP is constructed only because
/// Ipv4Module's ctor takes it (it auto-registers proto 1 into the L4 table).
struct Fixture {
    NoL2Dev    dev;
    IcmpModule icmp;
    Ipv4Module ipv4;
    UdpModule  udp;
    NetStack   stack;
    explicit Fixture(Ipv4Addr local) : ipv4(icmp, nullptr) {
        stack.add_protocol(kEtherTypeIpv4, ipv4);
        ipv4.add_l4(kIpProtoUdp, udp);
        InDevice cfg{};
        cfg.local   = local;
        cfg.gateway = local;
        stack.attach(dev, cfg);
    }
};

/// Build a 20-byte IPv4 header (valid checksum) wrapping @p l4 + emit the packet.
std::vector<uint8_t> ip_packet(Ipv4Addr src, Ipv4Addr dst, uint8_t proto,
                               const std::vector<uint8_t>& l4) {
    std::vector<uint8_t> p(20 + l4.size(), 0);
    p[0]                 = 0x45;  // version 4, IHL 5
    const uint16_t total = static_cast<uint16_t>(20 + l4.size());
    p[2]                 = static_cast<uint8_t>(total >> 8);
    p[3]                 = static_cast<uint8_t>(total & 0xFF);
    p[8]                 = 64;  // TTL
    p[9]                 = proto;
    for (int i = 0; i < 4; ++i) {
        p[12 + i] = src.oct[i];
        p[16 + i] = dst.oct[i];
    }
    std::memcpy(p.data() + 20, l4.data(), l4.size());
    const uint16_t cs = internet_checksum(p.data(), 20);
    p[10]             = static_cast<uint8_t>(cs >> 8);
    p[11]             = static_cast<uint8_t>(cs & 0xFF);
    return p;
}

/// Feed @p pkt back through the stack as a received frame, then drain one poll.
void deliver(Fixture& f, const std::vector<uint8_t>& pkt) {
    f.dev.rx.push_back(pkt);
    f.stack.poll();
}

}  // namespace

// ============================================================
// Header parse/build
// ============================================================

TEST("udp_header: build then parse round-trips every field (big-endian wire)") {
    UdpHeader in{};
    in.src_port = 0x1234;
    in.dst_port = 0xBEEF;
    in.length   = 100;
    in.checksum = 0xF00D;
    uint8_t wire[8];
    build_udp_header(in, wire);
    ASSERT_EQ(wire[0], 0x12u);
    ASSERT_EQ(wire[1], 0x34u);  // src_port big-endian
    ASSERT_EQ(wire[2], 0xBEu);
    ASSERT_EQ(wire[3], 0xEFu);  // dst_port
    UdpHeader out;
    parse_udp(wire, out);
    ASSERT_EQ(out.src_port, 0x1234u);
    ASSERT_EQ(out.dst_port, 0xBEEFu);
    ASSERT_EQ(out.length, 100u);
    ASSERT_EQ(out.checksum, 0xF00Du);
}

// ============================================================
// TX wire format
// ============================================================

TEST("udp_send: emits proto=17 IP packet + correct UDP header + payload") {
    Fixture                    f{ip(10, 0, 2, 15)};
    Capture                    listener;
    const std::vector<uint8_t> msg = {'p', 'i', 'n', 'g'};
    ASSERT_TRUE(f.udp.bind(7777, listener));
    ASSERT_TRUE(
        f.udp.send(f.dev, ip(10, 0, 2, 2), 1234, 7777, msg.data(), msg.size(), f.ipv4, f.stack)
            .ok());
    ASSERT_EQ(f.dev.sent.size(), 1u);
    const auto& pkt = f.dev.sent[0];
    ASSERT_EQ(pkt.size(), 20u + 8u + 4u);  // IPv4 + UDP header + payload
    ASSERT_EQ(pkt[9], 17u);                // IPv4 proto = UDP
    ASSERT_EQ(pkt[12], 10u);               // src IP 10.0.2.15
    ASSERT_EQ(pkt[16], 10u);               // dst IP 10.0.2.2
    ASSERT_EQ(pkt[19], 2u);
    ASSERT_EQ(pkt[20], 0x04u);
    ASSERT_EQ(pkt[21], 0xD2u);  // src_port 1234
    ASSERT_EQ(pkt[22], 0x1Eu);
    ASSERT_EQ(pkt[23], 0x61u);                                        // dst_port 7777
    ASSERT_EQ(static_cast<unsigned>((pkt[24] << 8) | pkt[25]), 12u);  // UDP length = 8 + 4
    ASSERT_TRUE(pkt[26] != 0 || pkt[27] != 0);                        // computed checksum nonzero
    ASSERT_EQ(pkt[28], 'p');
    ASSERT_EQ(pkt[31], 'g');  // payload
}

// ============================================================
// Full round-trip: send -> IPv4 table -> handle -> listener
// ============================================================

TEST("udp: send -> IPv4 L4-table dispatch -> handle -> listener") {
    Fixture                    f{ip(127, 0, 0, 1)};
    Capture                    listener;
    const std::vector<uint8_t> msg = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(f.udp.bind(7777, listener));
    ASSERT_TRUE(
        f.udp.send(f.dev, ip(127, 0, 0, 1), 1234, 7777, msg.data(), msg.size(), f.ipv4, f.stack)
            .ok());
    deliver(f, f.dev.sent[0]);  // feed the captured IP packet back in

    ASSERT_EQ(listener.calls, 1u);
    ASSERT_EQ(listener.last_src_port, 1234u);
    ASSERT_EQ(listener.last_src, ip(127, 0, 0, 1));
    ASSERT_EQ(listener.last_payload, msg);
}

TEST("udp: corrupted payload fails the UDP checksum -> dropped") {
    Fixture                    f{ip(127, 0, 0, 1)};
    Capture                    listener;
    const std::vector<uint8_t> msg = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(f.udp.bind(7777, listener));
    ASSERT_TRUE(
        f.udp.send(f.dev, ip(127, 0, 0, 1), 1234, 7777, msg.data(), msg.size(), f.ipv4, f.stack)
            .ok());
    std::vector<uint8_t> pkt = f.dev.sent[0];
    pkt[28] ^= 0xFF;  // corrupt a UDP payload byte (past IP(20) + UDP hdr(8))
    deliver(f, pkt);

    ASSERT_EQ(listener.calls, 0u);  // checksum bad -> silent drop
}

TEST("udp: checksum=0 means 'no checksum' -> accepted") {
    Fixture f{ip(127, 0, 0, 1)};
    Capture listener;
    ASSERT_TRUE(f.udp.bind(7777, listener));
    // Hand-build a UDP datagram whose checksum field is 0 (RFC 768 "no checksum").
    std::vector<uint8_t> udp(8 + 3, 0);
    udp[0]  = 0x04;
    udp[1]  = 0xD2;  // src_port 1234
    udp[2]  = 0x1E;
    udp[3]  = 0x61;  // dst_port 7777
    udp[4]  = 0x00;
    udp[5]  = 11;  // length = 8 + 3
    udp[6]  = 0;
    udp[7]  = 0;  // checksum = 0
    udp[8]  = 'h';
    udp[9]  = 'i';
    udp[10] = '!';
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 17, udp));

    ASSERT_EQ(listener.calls, 1u);  // checksum=0 skips verification -> accepted
    ASSERT_EQ(listener.last_payload, (std::vector<uint8_t>{'h', 'i', '!'}));
}

// ============================================================
// Port demux
// ============================================================

TEST("udp: datagram to an unbound port is silently dropped") {
    Fixture                    f{ip(127, 0, 0, 1)};
    Capture                    listener;
    const std::vector<uint8_t> msg = {'x'};
    ASSERT_TRUE(f.udp.bind(7777, listener));
    ASSERT_TRUE(
        f.udp.send(f.dev, ip(127, 0, 0, 1), 1234, 9999, msg.data(), msg.size(), f.ipv4,
                   f.stack)
            .ok());  // 9999 unbound
    deliver(f, f.dev.sent[0]);
    ASSERT_EQ(listener.calls, 0u);
}

TEST("udp: demux delivers to the port-matched listener only") {
    Fixture                    f{ip(127, 0, 0, 1)};
    Capture                    a, b;
    const std::vector<uint8_t> msg = {'y'};
    ASSERT_TRUE(f.udp.bind(7000, a));
    ASSERT_TRUE(f.udp.bind(8000, b));
    ASSERT_TRUE(
        f.udp.send(f.dev, ip(127, 0, 0, 1), 1, 8000, msg.data(), msg.size(), f.ipv4, f.stack).ok());
    deliver(f, f.dev.sent[0]);
    ASSERT_EQ(a.calls, 0u);
    ASSERT_EQ(b.calls, 1u);
}

TEST("udp: unbind stops delivery to a previously bound port") {
    Fixture                    f{ip(127, 0, 0, 1)};
    Capture                    listener;
    const std::vector<uint8_t> msg = {'z'};
    ASSERT_TRUE(f.udp.bind(7777, listener));
    f.udp.unbind(7777);
    ASSERT_TRUE(
        f.udp.send(f.dev, ip(127, 0, 0, 1), 1234, 7777, msg.data(), msg.size(), f.ipv4, f.stack)
            .ok());
    deliver(f, f.dev.sent[0]);
    ASSERT_EQ(listener.calls, 0u);
}

TEST("udp: bind rejects a duplicate port") {
    UdpModule udp;
    Capture   a, b;
    ASSERT_TRUE(udp.bind(7777, a));
    ASSERT_FALSE(udp.bind(7777, b));  // already bound
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
