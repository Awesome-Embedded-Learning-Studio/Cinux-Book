/**
 * @file test/unit/test_net_tcp.cpp
 * @brief Host unit tests for the TCP wire layer + the inbound checksum gate
 *        (batch 1).  TcpModule is registered as L4Handler proto 6; a hand-built
 *        TCP segment over IPv4 is fed through the real Ipv4Module dispatch and
 *        the checksum gate's diagnostics are inspected.  The connection FSM is
 *        exercised in batches 2-3.
 *
 * Links the real tcp.cpp + ipv4.cpp + icmp.cpp + arp.cpp + net_stack.cpp +
 * Cinux-Base checksum.cpp (pure logic, host-linkable).
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
#include "kernel/net/tcp.hpp"
#include "test_framework.h"

using cinux::lib::internet_checksum;
using cinux::net::InDevice;
using cinux::net::Ipv4Addr;
using cinux::net::Ipv4Module;
using cinux::net::IcmpModule;
using cinux::net::kEtherTypeIpv4;
using cinux::net::kIpProtoTcp;
using cinux::net::NetDevice;
using cinux::net::NetStack;
using cinux::net::Packet;
using cinux::net::TcpHeader;
using cinux::net::TcpModule;

namespace {

Ipv4Addr ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return Ipv4Addr{{a, b, c, d}};
}

/// No-L2 mock (same shape as test_net_udp): yields queued RX frames and captures
/// every send_l3 as an IP packet in `sent`.  Copy device (sink==nullptr) -- frame
/// bytes live in the vectors for the test's lifetime, so handlers can borrow them.
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

/// Stack + modules wired for a no-L2 device at @p local.  TCP joins ICMP (auto)
/// in the L4 table via add_l4(proto 6).
struct Fixture {
    NoL2Dev    dev;
    IcmpModule icmp;
    Ipv4Module ipv4;
    TcpModule  tcp;
    NetStack   stack;
    explicit Fixture(Ipv4Addr local) : ipv4(icmp, nullptr) {
        stack.add_protocol(kEtherTypeIpv4, ipv4);
        ipv4.add_l4(kIpProtoTcp, tcp);
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

/// Embed the TCP pseudo-header checksum into @p seg's checksum field (bytes
/// 16-17).  Mirrors the TX path that TcpModule::send_segment will use (batch 2).
void embed_tcp_checksum(std::vector<uint8_t>& seg, Ipv4Addr src, Ipv4Addr dst) {
    std::vector<uint8_t> buf(12 + seg.size(), 0);
    for (int i = 0; i < 4; ++i) {
        buf[i]     = src.oct[i];
        buf[4 + i] = dst.oct[i];
    }
    buf[9]  = kIpProtoTcp;
    buf[10] = static_cast<uint8_t>(seg.size() >> 8);
    buf[11] = static_cast<uint8_t>(seg.size() & 0xFF);
    std::memcpy(buf.data() + 12, seg.data(), seg.size());  // checksum field still 0
    uint16_t cs = internet_checksum(buf.data(), buf.size());
    if (cs == 0) {
        cs = 0xFFFF;  // TCP mandates a nonzero checksum
    }
    seg[16] = static_cast<uint8_t>(cs >> 8);
    seg[17] = static_cast<uint8_t>(cs & 0xFF);
}

/// Build a checksummed TCP segment (20-byte header + optional payload).
std::vector<uint8_t> tcp_segment(Ipv4Addr src, Ipv4Addr dst, uint16_t sp, uint16_t dp, uint32_t seq,
                                 uint32_t ack, uint8_t flags,
                                 const std::vector<uint8_t>& data = {}) {
    std::vector<uint8_t> seg(20 + data.size(), 0);
    seg[0]  = static_cast<uint8_t>(sp >> 8);
    seg[1]  = static_cast<uint8_t>(sp & 0xFF);
    seg[2]  = static_cast<uint8_t>(dp >> 8);
    seg[3]  = static_cast<uint8_t>(dp & 0xFF);
    seg[4]  = static_cast<uint8_t>(seq >> 24);
    seg[5]  = static_cast<uint8_t>(seq >> 16);
    seg[6]  = static_cast<uint8_t>(seq >> 8);
    seg[7]  = static_cast<uint8_t>(seq & 0xFF);
    seg[8]  = static_cast<uint8_t>(ack >> 24);
    seg[9]  = static_cast<uint8_t>(ack >> 16);
    seg[10] = static_cast<uint8_t>(ack >> 8);
    seg[11] = static_cast<uint8_t>(ack & 0xFF);
    seg[12] = static_cast<uint8_t>(5 << 4);  // data offset = 5 (20 bytes)
    seg[13] = flags;
    seg[14] = 0x20;  // window = 8192 (arbitrary nonzero)
    seg[15] = 0x00;
    // checksum bytes 16-17 left 0 for embed_tcp_checksum
    if (!data.empty()) {
        std::memcpy(seg.data() + 20, data.data(), data.size());
    }
    embed_tcp_checksum(seg, src, dst);
    return seg;
}

/// Feed @p pkt back through the stack as a received frame, then drain one poll.
void deliver(Fixture& f, const std::vector<uint8_t>& pkt) {
    f.dev.rx.push_back(pkt);
    f.stack.poll();
}

/// Parse the TCP header out of a captured IP packet (skip the 20-byte IPv4
/// header).  Returns false if @p pkt is too short.
bool seg_tcp(const std::vector<uint8_t>& pkt, TcpHeader& out) {
    if (pkt.size() < 20 + 20) {
        return false;
    }
    parse_tcp(pkt.data() + 20, out);
    return true;
}

/// Capture listener: records on_accept/on_data/on_close invocations.
struct Capture : cinux::net::TcpListener {
    uint32_t                accepts = 0;
    uint32_t                closes  = 0;
    uint32_t                datas   = 0;
    cinux::net::TcpEndpoint last_remote{};
    std::vector<uint8_t>    last_data;
    void                    on_accept(const cinux::net::TcpEndpoint& /*local*/,
                                      const cinux::net::TcpEndpoint& remote) override {
        ++accepts;
        last_remote = remote;
    }
    void on_data(const cinux::net::TcpEndpoint& /*local*/,
                 const cinux::net::TcpEndpoint& /*remote*/, cinux::net::FrameView data) override {
        ++datas;
        last_data.assign(data.data(), data.data() + data.size());
    }
    void on_close(const cinux::net::TcpEndpoint& /*local*/,
                  const cinux::net::TcpEndpoint& /*remote*/) override {
        ++closes;
    }
};

/// Drive a full 3-way handshake to ESTABLISHED on both ends: server listens on
/// 7777, client connects from 1234, each captured segment is fed back.  Leaves
/// client (1234->7777) and server (7777->1234) ESTABLISHED.  Used by the
/// data/close tests so they start from a live connection.
void do_handshake(Fixture& f, Capture& server_l) {
    ASSERT_TRUE(f.tcp.listen(7777, server_l));
    ASSERT_TRUE(f.tcp.connect(f.dev, 1234, ip(127, 0, 0, 1), 7777, f.ipv4, f.stack).ok());  // SYN
    deliver(f, f.dev.sent[0]);  // -> SYN-ACK (server SynReceived)
    deliver(f, f.dev.sent[1]);  // -> ACK (client Established)
    deliver(f, f.dev.sent[2]);  // -> server Established + on_accept
}

}  // namespace

// ============================================================
// Header parse/build
// ============================================================

TEST("tcp_header: build then parse round-trips every field (big-endian wire)") {
    TcpHeader in{};
    in.src_port   = 0x1234;
    in.dst_port   = 0xBEEF;
    in.seq        = 0x11223344;
    in.ack        = 0xDEADBEEF;
    in.data_off   = 5;
    in.flags      = 0x18;  // PSH|ACK
    in.window     = 0xFFFF;
    in.checksum   = 0xF00D;
    in.urgent_ptr = 0x0001;
    uint8_t wire[20];
    build_tcp_header(in, wire);
    // data offset sits in the hi nibble of byte 12; flags in byte 13; seq/ack
    // are 32-bit big-endian.
    ASSERT_EQ(wire[12], 0x50u);
    ASSERT_EQ(wire[13], 0x18u);
    ASSERT_EQ(wire[4], 0x11u);   // seq top byte
    ASSERT_EQ(wire[11], 0xEFu);  // ack low byte
    TcpHeader out;
    parse_tcp(wire, out);
    ASSERT_EQ(out.src_port, 0x1234u);
    ASSERT_EQ(out.dst_port, 0xBEEFu);
    ASSERT_EQ(out.seq, 0x11223344u);
    ASSERT_EQ(out.ack, 0xDEADBEEFu);
    ASSERT_EQ(out.data_off, 5u);
    ASSERT_EQ(out.flags, 0x18u);
    ASSERT_EQ(out.window, 0xFFFFu);
    ASSERT_EQ(out.checksum, 0xF00Du);
    ASSERT_EQ(out.urgent_ptr, 0x0001u);
    ASSERT_EQ(tcp_header_bytes(out), 20u);
}

// ============================================================
// Inbound checksum gate (the batch-1 handle() observable)
// ============================================================

TEST("tcp_handle: a valid segment passes the checksum gate + is recorded") {
    Fixture f{ip(127, 0, 0, 1)};
    auto    seg =
        tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 1234, 7777, 1000, 2000, 0x10 /*ACK*/);
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, seg));

    ASSERT_EQ(f.tcp.valid_count(), 1u);
    ASSERT_EQ(f.tcp.last_src_port(), 1234u);
    ASSERT_EQ(f.tcp.last_dst_port(), 7777u);
    ASSERT_EQ(f.tcp.last_seq(), 1000u);
    ASSERT_EQ(f.tcp.last_ack(), 2000u);
    ASSERT_EQ(f.tcp.last_flags(), 0x10u);
}

TEST("tcp_handle: a corrupt segment fails the checksum gate -> dropped") {
    Fixture f{ip(127, 0, 0, 1)};
    auto    seg =
        tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 1234, 7777, 1000, 2000, 0x10, {'h', 'i'});
    seg[20] ^= 0xFF;  // corrupt the first data byte (covered by the checksum)
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, seg));

    ASSERT_EQ(f.tcp.valid_count(), 0u);  // bad checksum -> silent drop
}

TEST("tcp_handle: proto 6 reaches TcpModule via the L4 dispatch table") {
    Fixture f{ip(127, 0, 0, 1)};
    auto    syn =
        tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 4321, 80, 0x40000000, 0, 0x02 /*SYN*/);
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, syn));

    ASSERT_EQ(f.tcp.valid_count(), 1u);
    ASSERT_EQ(f.tcp.last_flags(), 0x02u);  // SYN dispatched to TCP, not eaten elsewhere
}

// ============================================================
// 3-way handshake (connection table + seq/ACK arithmetic)
// ============================================================

TEST("tcp_handshake: 3-way handshake -> both ends ESTABLISHED, seq/ack correct") {
    Fixture f{ip(127, 0, 0, 1)};
    Capture server_l;
    ASSERT_TRUE(f.tcp.listen(7777, server_l));

    // Client active open -> SYN (captured as an IP packet).
    ASSERT_TRUE(f.tcp.connect(f.dev, 1234, ip(127, 0, 0, 1), 7777, f.ipv4, f.stack).ok());
    ASSERT_EQ(f.dev.sent.size(), 1u);
    ASSERT_EQ(f.tcp.connection_count(), 1u);  // client conn
    ASSERT_EQ(f.tcp.state_of(1234, ip(127, 0, 0, 1), 7777), cinux::net::TcpState::kSynSent);

    TcpHeader syn;
    ASSERT_TRUE(seg_tcp(f.dev.sent[0], syn));
    ASSERT_EQ(syn.flags, 0x02u);  // SYN
    ASSERT_EQ(syn.src_port, 1234u);
    ASSERT_EQ(syn.dst_port, 7777u);
    ASSERT_EQ(syn.ack, 0u);
    const uint32_t s_c = syn.seq;  // client ISN

    // Deliver the SYN -> server passive open -> SYN-ACK.
    deliver(f, f.dev.sent[0]);
    ASSERT_EQ(f.tcp.connection_count(), 2u);  // + server conn
    ASSERT_EQ(f.tcp.state_of(7777, ip(127, 0, 0, 1), 1234), cinux::net::TcpState::kSynReceived);
    ASSERT_EQ(f.dev.sent.size(), 2u);

    TcpHeader synack;
    ASSERT_TRUE(seg_tcp(f.dev.sent[1], synack));
    ASSERT_EQ(synack.flags, 0x12u);   // SYN|ACK
    ASSERT_EQ(synack.ack, s_c + 1);   // the peer's SYN consumed one sequence number
    const uint32_t s_s = synack.seq;  // server ISN

    // Deliver the SYN-ACK -> client ACKs, reaches ESTABLISHED.
    deliver(f, f.dev.sent[1]);
    ASSERT_EQ(f.tcp.state_of(1234, ip(127, 0, 0, 1), 7777), cinux::net::TcpState::kEstablished);
    ASSERT_EQ(f.dev.sent.size(), 3u);

    TcpHeader ack3;
    ASSERT_TRUE(seg_tcp(f.dev.sent[2], ack3));
    ASSERT_EQ(ack3.flags, 0x10u);  // ACK
    ASSERT_EQ(ack3.seq, s_c + 1);  // our SYN consumed one
    ASSERT_EQ(ack3.ack, s_s + 1);  // the peer's SYN consumed one

    // Deliver the ACK -> server ESTABLISHED + on_accept fires.
    deliver(f, f.dev.sent[2]);
    ASSERT_EQ(f.tcp.state_of(7777, ip(127, 0, 0, 1), 1234), cinux::net::TcpState::kEstablished);
    ASSERT_EQ(server_l.accepts, 1u);
    ASSERT_EQ(server_l.last_remote.port, 1234u);
}

TEST("tcp_handshake: SYN to a closed (unlistened) port is answered with RST") {
    Fixture f{ip(127, 0, 0, 1)};
    // No listener on 9999 -- hand-build a SYN to it.
    auto    syn = tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 1234, 9999, 5000, 0, 0x02);
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, syn));

    ASSERT_EQ(f.dev.sent.size(), 1u);  // the RST
    TcpHeader rst;
    ASSERT_TRUE(seg_tcp(f.dev.sent[0], rst));
    ASSERT_EQ(rst.flags, 0x14u);              // RST|ACK
    ASSERT_EQ(rst.ack, 5001u);                // ack = SEG.SEQ + 1 (RFC 793)
    ASSERT_EQ(f.tcp.connection_count(), 0u);  // no connection created
}

TEST("tcp_connect: a duplicate 4-tuple is rejected") {
    Fixture f{ip(127, 0, 0, 1)};
    ASSERT_TRUE(f.tcp.connect(f.dev, 1234, ip(127, 0, 0, 1), 7777, f.ipv4, f.stack).ok());
    ASSERT_FALSE(f.tcp.connect(f.dev, 1234, ip(127, 0, 0, 1), 7777, f.ipv4, f.stack).ok());
}

// ============================================================
// Data transfer + 4-way teardown
// ============================================================

TEST("tcp_data: established connection delivers in-order data + ACKs") {
    Fixture f{ip(127, 0, 0, 1)};
    Capture server_l;
    do_handshake(f, server_l);
    const std::vector<uint8_t> msg = {'h', 'e', 'l', 'l', 'o'};

    // Client sends data -> PSH|ACK segment.
    ASSERT_TRUE(
        f.tcp.send(f.dev, 1234, ip(127, 0, 0, 1), 7777, msg.data(), msg.size(), f.ipv4, f.stack)
            .ok());
    ASSERT_EQ(f.dev.sent.size(), 4u);  // 3 handshake + 1 data
    TcpHeader dseg;
    ASSERT_TRUE(seg_tcp(f.dev.sent[3], dseg));
    ASSERT_EQ(dseg.flags, 0x18u);  // PSH|ACK

    // Server receives -> on_data + ACK whose ack == data seq + len (rcv_nxt advanced).
    deliver(f, f.dev.sent[3]);
    ASSERT_EQ(server_l.datas, 1u);
    ASSERT_EQ(server_l.last_data, msg);
    ASSERT_EQ(f.dev.sent.size(), 5u);  // + server ACK
    TcpHeader sack;
    ASSERT_TRUE(seg_tcp(f.dev.sent[4], sack));
    ASSERT_EQ(sack.flags, 0x10u);  // ACK
    ASSERT_EQ(sack.ack, dseg.seq + static_cast<uint32_t>(msg.size()));
}

TEST("tcp_close: 4-way teardown -> both connections CLOSED") {
    Fixture f{ip(127, 0, 0, 1)};
    Capture server_l;
    do_handshake(f, server_l);

    // Client active close -> FIN.
    ASSERT_TRUE(f.tcp.close(f.dev, 1234, ip(127, 0, 0, 1), 7777, f.ipv4, f.stack).ok());
    ASSERT_EQ(f.tcp.state_of(1234, ip(127, 0, 0, 1), 7777), cinux::net::TcpState::kFinWait1);
    const size_t fin_idx = f.dev.sent.size() - 1;
    TcpHeader    fin;
    ASSERT_TRUE(seg_tcp(f.dev.sent[fin_idx], fin));
    ASSERT_EQ(fin.flags, 0x11u);  // FIN|ACK

    // Server rx FIN -> ACK + on_close -> CLOSE_WAIT.
    deliver(f, f.dev.sent[fin_idx]);
    ASSERT_EQ(server_l.closes, 1u);
    ASSERT_EQ(f.tcp.state_of(7777, ip(127, 0, 0, 1), 1234), cinux::net::TcpState::kCloseWait);
    const size_t ack_idx = f.dev.sent.size() - 1;  // server's ACK for the client FIN

    // Client rx ACK -> FIN_WAIT_2.
    deliver(f, f.dev.sent[ack_idx]);
    ASSERT_EQ(f.tcp.state_of(1234, ip(127, 0, 0, 1), 7777), cinux::net::TcpState::kFinWait2);

    // Server app closes -> FIN -> LAST_ACK.
    ASSERT_TRUE(f.tcp.close(f.dev, 7777, ip(127, 0, 0, 1), 1234, f.ipv4, f.stack).ok());
    ASSERT_EQ(f.tcp.state_of(7777, ip(127, 0, 0, 1), 1234), cinux::net::TcpState::kLastAck);
    const size_t sfin_idx = f.dev.sent.size() - 1;

    // Client rx server FIN -> ACK -> CLOSED.
    deliver(f, f.dev.sent[sfin_idx]);
    ASSERT_EQ(f.tcp.state_of(1234, ip(127, 0, 0, 1), 7777), cinux::net::TcpState::kClosed);
    const size_t final_ack_idx = f.dev.sent.size() - 1;

    // Server rx final ACK -> CLOSED.  Both connections reaped.
    deliver(f, f.dev.sent[final_ack_idx]);
    ASSERT_EQ(f.tcp.state_of(7777, ip(127, 0, 0, 1), 1234), cinux::net::TcpState::kClosed);
    ASSERT_EQ(f.tcp.connection_count(), 0u);
}

// ============================================================
// Adversarial input -- malformed segments are safely dropped (no crash, no
// spurious state).  Confidence for when M6 makes TCP reachable on real traffic.
// ============================================================

TEST("tcp_adversarial: a truncated header (< 20 bytes) is dropped") {
    Fixture                    f{ip(127, 0, 0, 1)};
    const std::vector<uint8_t> short_seg(10, 0);  // too short for a TCP header
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, short_seg));
    ASSERT_EQ(f.tcp.valid_count(), 0u);  // never passed the size gate
    ASSERT_EQ(f.tcp.connection_count(), 0u);
}

TEST("tcp_adversarial: data_off claiming more than delivered is dropped") {
    Fixture f{ip(127, 0, 0, 1)};
    Capture server_l;
    f.tcp.listen(7777, server_l);
    // SYN with data_off = 15 (claims a 60-byte header) but only 20 bytes on the
    // wire.  Must drop, not allocate a connection from the SYN.
    auto seg = tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 1234, 7777, 100, 0, 0x02);
    seg[12]  = static_cast<uint8_t>(15 << 4);  // data_off = 15 -> 60 bytes (> 20 delivered)
    seg[16]  = 0;
    seg[17]  = 0;  // re-checksum over the modified 20 real bytes
    embed_tcp_checksum(seg, ip(127, 0, 0, 1), ip(127, 0, 0, 1));
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, seg));
    ASSERT_EQ(f.tcp.connection_count(), 0u);  // malformed -> dropped
    ASSERT_EQ(server_l.accepts, 0u);
}

TEST("tcp_adversarial: data_off < 5 (under-min header) is dropped") {
    Fixture f{ip(127, 0, 0, 1)};
    auto seg = tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 1234, 7777, 100, 200, 0x10 /*ACK*/);
    seg[12]  = static_cast<uint8_t>(0 << 4);  // data_off = 0 -- impossible (< 20 bytes)
    seg[16]  = 0;
    seg[17]  = 0;
    embed_tcp_checksum(seg, ip(127, 0, 0, 1), ip(127, 0, 0, 1));
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, seg));
    ASSERT_EQ(f.tcp.connection_count(), 0u);  // would have mis-read header as payload
}

TEST("tcp_adversarial: connection-table overflow drops the 9th SYN (no crash)") {
    Fixture f{ip(127, 0, 0, 1)};
    Capture server_l;
    ASSERT_TRUE(f.tcp.listen(7777, server_l));
    // 8 SYNs from distinct remotes fill the table (kMaxTcpCons = 8).
    for (uint16_t i = 0; i < 8; ++i) {
        auto syn = tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), static_cast<uint16_t>(1000 + i),
                               7777, static_cast<uint32_t>(1000 + i), 0, 0x02);
        deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, syn));
    }
    ASSERT_EQ(f.tcp.connection_count(), 8u);
    // 9th SYN from yet another remote -> table full -> dropped, no crash.
    auto syn9 = tcp_segment(ip(10, 1, 2, 3), ip(127, 0, 0, 1), 9999, 7777, 999, 0, 0x02);
    deliver(f, ip_packet(ip(10, 1, 2, 3), ip(127, 0, 0, 1), 6, syn9));
    ASSERT_EQ(f.tcp.connection_count(), 8u);  // 9th not added
}

TEST("tcp_adversarial: a stray ACK to no connection is silently dropped") {
    Fixture f{ip(127, 0, 0, 1)};
    auto    ack = tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 1234, 9999, 100, 200, 0x10);
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, ack));
    ASSERT_EQ(f.tcp.connection_count(), 0u);
    ASSERT_EQ(f.dev.sent.size(), 0u);  // not a SYN -> no RST, just a silent drop
}

TEST("tcp_adversarial: out-of-order data on an established conn is dropped") {
    Fixture f{ip(127, 0, 0, 1)};
    Capture server_l;
    do_handshake(f, server_l);
    // The client ISN is in the captured SYN (dev.sent[0]); craft data with a SEQ
    // far from the server's RCV.NXT.
    TcpHeader syn;
    ASSERT_TRUE(seg_tcp(f.dev.sent[0], syn));
    const uint32_t bad_seq = syn.seq + 1000;
    auto           data =
        tcp_segment(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 1234, 7777, bad_seq, 0, 0x18, {'x'});
    deliver(f, ip_packet(ip(127, 0, 0, 1), ip(127, 0, 0, 1), 6, data));
    ASSERT_EQ(server_l.datas, 0u);  // out of order -> no on_data
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
