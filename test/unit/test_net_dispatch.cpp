/**
 * @file test/unit/test_net_dispatch.cpp
 * @brief Host unit tests for NetStack dispatch, L2 parse, and the buffer
 *        recycle contract (the lifetime red-teams: handle / drop / runt /
 *        no-L2, with a counting BufferSink).
 *
 * Links the real kernel/net/net_stack.cpp (pure logic, host-linkable).  A mock
 * NetDevice yields crafted frames; a mock ProtocolHandler records hits; a
 * counting BufferSink asserts recycle-on-every-exit.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL

#include <cinux/expected.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kernel/net/net_device.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/net/protocol_handler.hpp"
#include "test_framework.h"

using cinux::lib::ErrorOr;
using cinux::net::BufferSink;
using cinux::net::EthAddr;
using cinux::net::FrameView;
using cinux::net::InDevice;
using cinux::net::L2Info;
using cinux::net::NetDevice;
using cinux::net::NetStack;
using cinux::net::Packet;
using cinux::net::ProtocolHandler;

namespace {

/// BufferSink that counts recycle() calls (verifies the scope-guard contract).
struct CountingSink : BufferSink {
    int            calls = 0;
    const uint8_t* last  = nullptr;
    void           recycle(const uint8_t* data) override {
        ++calls;
        last = data;
    }
};

/// Mock NIC: yields queued frames (with an optional sink per frame) and
/// captures send_l3() calls. Frame bytes live in the mock for the test's life.
class MockNetDevice : public NetDevice {
public:
    bool with_eth = true;

    std::vector<std::vector<uint8_t>> yield_bytes;
    std::vector<BufferSink*>          yield_sinks;
    size_t                            idx = 0;

    std::vector<std::vector<uint8_t>> sent;  // captured send_l3 payloads

    bool mac(EthAddr& out) const override {
        for (int i = 0; i < 6; ++i) {
            out.oct[i] = 0x52;
        }
        return true;
    }
    bool has_ethernet_header() const override { return with_eth; }

    bool poll_rx(Packet& out) override {
        if (idx >= yield_bytes.size()) {
            return false;
        }
        out.data = yield_bytes[idx].data();
        out.len  = static_cast<uint32_t>(yield_bytes[idx].size());
        out.sink = (idx < yield_sinks.size()) ? yield_sinks[idx] : nullptr;
        ++idx;
        return true;
    }

    ErrorOr<void> send_l3(const EthAddr&, uint16_t, const uint8_t* l3, uint32_t len) override {
        sent.emplace_back(l3, l3 + len);
        return {};
    }

    void queue(std::vector<uint8_t> bytes, BufferSink* sink = nullptr) {
        yield_bytes.push_back(std::move(bytes));
        yield_sinks.push_back(sink);
    }
};

/// Mock protocol handler: records the last on_frame() invocation.
struct MockHandler : ProtocolHandler {
    int        calls            = 0;
    uint16_t   last_ethertype   = 0;
    size_t     last_payload_len = 0;
    NetDevice* last_dev         = nullptr;

    void on_frame(const L2Info& l2, FrameView payload, NetDevice& dev, NetStack&) override {
        ++calls;
        last_ethertype   = l2.ethertype;
        last_payload_len = payload.size();
        last_dev         = &dev;
    }
};

/// Build an Ethernet II frame: broadcast dst, fixed src, given ethertype + payload.
std::vector<uint8_t> eth_frame(uint16_t ethertype, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f(14 + payload.size(), 0);
    for (int i = 0; i < 6; ++i) {
        f[i]     = 0xFF;  // dst broadcast
        f[6 + i] = 0x52;  // src
    }
    f[12] = static_cast<uint8_t>(ethertype >> 8);
    f[13] = static_cast<uint8_t>(ethertype & 0xFF);
    if (!payload.empty()) {
        std::memcpy(f.data() + 14, payload.data(), payload.size());
    }
    return f;
}

}  // namespace

// ============================================================
// Dispatch + L2 parse
// ============================================================

TEST("net_dispatch: ARP frame routes to the ARP handler") {
    MockNetDevice dev;
    MockHandler   arp;
    NetStack      stack;
    stack.add_protocol(cinux::net::kEtherTypeArp, arp);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    std::vector<uint8_t> body = {0xAA, 0xBB, 0xCC};
    dev.queue(eth_frame(cinux::net::kEtherTypeArp, body));
    stack.poll();

    ASSERT_EQ(arp.calls, 1);
    ASSERT_EQ(arp.last_ethertype, 0x0806u);
    ASSERT_EQ(arp.last_payload_len, 3u);
    ASSERT_TRUE(arp.last_dev == &dev);
}

TEST("net_dispatch: frame with no matching handler is silently dropped") {
    MockNetDevice dev;
    MockHandler   arp;
    NetStack      stack;
    stack.add_protocol(cinux::net::kEtherTypeArp, arp);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    dev.queue(eth_frame(cinux::net::kEtherTypeIpv4, {0x45, 0x00}));  // IPv4, no handler
    stack.poll();

    ASSERT_EQ(arp.calls, 0);  // dropped, no false hit
}

TEST("net_dispatch: IPv4 frame routes to the IPv4 handler (ethertype select)") {
    MockNetDevice dev;
    MockHandler   arp, ip;
    NetStack      stack;
    stack.add_protocol(cinux::net::kEtherTypeArp, arp);
    stack.add_protocol(cinux::net::kEtherTypeIpv4, ip);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    dev.queue(eth_frame(cinux::net::kEtherTypeIpv4, {0x45, 0x00, 0x00, 0x14}));
    stack.poll();

    ASSERT_EQ(ip.calls, 1);
    ASSERT_EQ(arp.calls, 0);  // ARP handler untouched
    ASSERT_EQ(ip.last_ethertype, 0x0800u);
}

// ============================================================
// Buffer recycle contract (lifetime red-teams)
// ============================================================

TEST("net_buffer: handled frame recycles the buffer exactly once") {
    MockNetDevice dev;
    MockHandler   arp;
    CountingSink  sink;
    NetStack      stack;
    stack.add_protocol(cinux::net::kEtherTypeArp, arp);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    dev.queue(eth_frame(cinux::net::kEtherTypeArp, {1, 2, 3}), &sink);
    stack.poll();

    ASSERT_EQ(arp.calls, 1);
    ASSERT_EQ(sink.calls, 1);  // recycled exactly once after dispatch
}

TEST("net_buffer: dropped frame (no handler) still recycles") {
    MockNetDevice dev;
    CountingSink  sink;
    NetStack      stack;
    ASSERT_TRUE(stack.attach(dev, InDevice{}));  // no protocols registered

    dev.queue(eth_frame(cinux::net::kEtherTypeIpv4, {0x45}), &sink);
    stack.poll();

    ASSERT_EQ(sink.calls, 1);  // not leaked -- guard fires on drop
}

TEST("net_buffer: runt frame (<14B) is dropped and still recycles") {
    MockNetDevice dev;
    MockHandler   arp;
    CountingSink  sink;
    NetStack      stack;
    stack.add_protocol(cinux::net::kEtherTypeArp, arp);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    std::vector<uint8_t> runt(10, 0xEE);  // < 14B Ethernet header
    dev.queue(std::move(runt), &sink);
    stack.poll();

    ASSERT_EQ(arp.calls, 0);   // runt -> parse fails -> drop
    ASSERT_EQ(sink.calls, 1);  // guard still recycles
}

TEST("net_buffer: copy device (sink==nullptr) is never dereferenced") {
    MockNetDevice dev;
    MockHandler   arp;
    NetStack      stack;
    stack.add_protocol(cinux::net::kEtherTypeArp, arp);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    dev.queue(eth_frame(cinux::net::kEtherTypeArp, {9, 9}), nullptr);  // sink null
    stack.poll();  // must not crash on the null-sink path

    ASSERT_EQ(arp.calls, 1);
}

// ============================================================
// No-L2 (loopback-style) dispatch
// ============================================================

TEST("net_dispatch: no-L2 device derives IPv4 from version nibble") {
    MockNetDevice dev;
    dev.with_eth = false;
    MockHandler ip;
    NetStack    stack;
    stack.add_protocol(cinux::net::kEtherTypeIpv4, ip);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    // Raw IPv4: first nibble 4 (version 4). Whole frame = payload.
    std::vector<uint8_t> raw_ip = {0x45, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00};
    dev.queue(std::move(raw_ip));
    stack.poll();

    ASSERT_EQ(ip.calls, 1);
    ASSERT_EQ(ip.last_ethertype, 0x0800u);
    ASSERT_EQ(ip.last_payload_len, 8u);
}

TEST("net_dispatch: two frames in one poll drain in order") {
    MockNetDevice dev;
    MockHandler   arp;
    NetStack      stack;
    stack.add_protocol(cinux::net::kEtherTypeArp, arp);
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    dev.queue(eth_frame(cinux::net::kEtherTypeArp, {1}));
    dev.queue(eth_frame(cinux::net::kEtherTypeArp, {2, 2}));
    stack.poll();

    ASSERT_EQ(arp.calls, 2);
    ASSERT_EQ(arp.last_payload_len, 2u);  // second frame handled last
}

TEST("net_dispatch: send_l3 forwards to the egress device") {
    MockNetDevice dev;
    NetStack      stack;
    ASSERT_TRUE(stack.attach(dev, InDevice{}));

    EthAddr dst{};
    for (int i = 0; i < 6; ++i) {
        dst.oct[i] = 0xFF;
    }
    auto r = stack.send_l3(dev, dst, cinux::net::kEtherTypeIpv4,
                           reinterpret_cast<const uint8_t*>("ping"), 4);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(dev.sent.size(), 1u);
    ASSERT_EQ(dev.sent[0].size(), 4u);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
