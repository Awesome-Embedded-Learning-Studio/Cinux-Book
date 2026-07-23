/**
 * @file test/unit/test_net_arp_module.cpp
 * @brief Host unit tests for ArpModule (request -> reply, reply -> cache,
 *        resolve_l3 miss -> async request).
 *
 * A mock Ethernet NetDevice yields crafted ARP frames and captures send_l3
 * replies. Links the real arp.cpp + net_stack.cpp (pure logic, host-linkable).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL

#include <cinux/expected.hpp>
#include <cstdint>
#include <vector>

#include "kernel/net/arp.hpp"
#include "kernel/net/net_device.hpp"
#include "kernel/net/net_stack.hpp"
#include "test_framework.h"

using cinux::lib::ErrorOr;
using cinux::net::ArpModule;
using cinux::net::build_arp;
using cinux::net::EthAddr;
using cinux::net::InDevice;
using cinux::net::Ipv4Addr;
using cinux::net::kArpReply;
using cinux::net::kArpRequest;
using cinux::net::kEtherTypeArp;
using cinux::net::L2Info;
using cinux::net::NetDevice;
using cinux::net::NetStack;
using cinux::net::Packet;
using cinux::net::parse_arp;
using cinux::net::ArpPacket;

namespace {

EthAddr mac(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f) {
    return EthAddr{{a, b, c, d, e, f}};
}
Ipv4Addr ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return Ipv4Addr{{a, b, c, d}};
}

class MockEth : public NetDevice {
public:
    // Direct init: the member override mac(EthAddr&) would hide a file-scope
    // mac() helper inside this class, so spell the MAC literally here.
    EthAddr my_mac = EthAddr{{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};

    struct Sent {
        EthAddr              next_hop;
        uint16_t             ethertype;
        std::vector<uint8_t> bytes;
    };
    std::vector<Sent>                 sent;
    std::vector<std::vector<uint8_t>> rx;
    size_t                            idx = 0;

    bool mac(EthAddr& out) const override {
        out = my_mac;
        return true;
    }
    bool has_ethernet_header() const override { return true; }
    bool poll_rx(Packet& out) override {
        if (idx >= rx.size()) {
            return false;
        }
        out.data = rx[idx].data();
        out.len  = static_cast<uint32_t>(rx[idx].size());
        out.sink = nullptr;  // copy device
        ++idx;
        return true;
    }
    ErrorOr<void> send_l3(const EthAddr& nh, uint16_t et, const uint8_t* l3,
                          uint32_t len) override {
        sent.push_back({nh, et, std::vector<uint8_t>(l3, l3 + len)});
        return {};
    }

    // Build an Ethernet-framed ARP request "who has tpa? tell spa (sha)".
    std::vector<uint8_t> make_arp_request(EthAddr sha, Ipv4Addr spa, Ipv4Addr tpa) {
        std::vector<uint8_t> f(14 + 28, 0);
        for (int i = 0; i < 6; ++i) {
            f[i]     = 0xFF;        // dst broadcast
            f[6 + i] = sha.oct[i];  // src
        }
        f[12] = 0x08;
        f[13] = 0x06;  // ethertype ARP
        ArpPacket p{};
        p.htype = 1;
        p.ptype = 0x0800;
        p.hlen  = 6;
        p.plen  = 4;
        p.oper  = kArpRequest;
        p.sha   = sha;
        p.spa   = spa;
        p.tha   = EthAddr{};
        p.tpa   = tpa;
        build_arp(p, f.data() + 14);
        return f;
    }
};

}  // namespace

// ============================================================
// ARP request -> reply
// ============================================================

TEST("arp_module: request for our IP -> unicast reply to requester") {
    MockEth   dev;
    ArpModule arp;
    NetStack  stack;
    ASSERT_TRUE(stack.attach(dev, InDevice{dev.my_mac, ip(10, 0, 2, 15), ip(10, 0, 2, 2)}));
    stack.add_protocol(kEtherTypeArp, arp);

    const EthAddr req_mac = mac(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
    dev.rx.push_back(dev.make_arp_request(req_mac, ip(10, 0, 2, 99), ip(10, 0, 2, 15)));
    stack.poll();

    ASSERT_EQ(dev.sent.size(), 1u);
    ASSERT_EQ(dev.sent[0].next_hop, req_mac);   // unicast to requester
    ASSERT_EQ(dev.sent[0].ethertype, 0x0806u);  // ARP
    // The 28-byte body is an ARP reply (op 2), sender = us, target = requester.
    ArpPacket r;
    parse_arp(dev.sent[0].bytes.data(), r);
    ASSERT_EQ(r.oper, kArpReply);
    ASSERT_EQ(r.sha, dev.my_mac);
    ASSERT_EQ(r.spa, ip(10, 0, 2, 15));
    ASSERT_EQ(r.tha, req_mac);
    ASSERT_EQ(r.tpa, ip(10, 0, 2, 99));
}

TEST("arp_module: request for a foreign IP -> no reply") {
    MockEth   dev;
    ArpModule arp;
    NetStack  stack;
    ASSERT_TRUE(stack.attach(dev, InDevice{dev.my_mac, ip(10, 0, 2, 15), ip(10, 0, 2, 2)}));
    stack.add_protocol(kEtherTypeArp, arp);

    dev.rx.push_back(
        dev.make_arp_request(mac(1, 2, 3, 4, 5, 6), ip(10, 0, 2, 99), ip(10, 0, 2, 200)));
    stack.poll();

    ASSERT_EQ(dev.sent.size(), 0u);  // not for us -> silent
}

// ============================================================
// ARP reply -> cache learns the sender
// ============================================================

TEST("arp_module: observed reply populates the cache") {
    MockEth   dev;
    ArpModule arp;
    NetStack  stack;
    ASSERT_TRUE(stack.attach(dev, InDevice{dev.my_mac, ip(10, 0, 2, 15), ip(10, 0, 2, 2)}));
    stack.add_protocol(kEtherTypeArp, arp);

    const EthAddr gw = mac(0x52, 0x55, 0x0A, 0x00, 0x02, 0x02);
    // Build a reply frame (op 2) "10.0.2.2 is at gw".
    ArpPacket     p{};
    p.htype = 1;
    p.ptype = 0x0800;
    p.hlen  = 6;
    p.plen  = 4;
    p.oper  = kArpReply;
    p.sha   = gw;
    p.spa   = ip(10, 0, 2, 2);
    p.tha   = dev.my_mac;
    p.tpa   = ip(10, 0, 2, 15);
    std::vector<uint8_t> f(14 + 28, 0);
    f[12] = 0x08;
    f[13] = 0x06;
    build_arp(p, f.data() + 14);
    dev.rx.push_back(std::move(f));
    stack.poll();

    EthAddr cached{};
    ASSERT_TRUE(arp.lookup(ip(10, 0, 2, 2), cached));
    ASSERT_EQ(cached, gw);
}

// ============================================================
// resolve_l3: miss -> async request; after cache -> hit
// ============================================================

TEST("arp_module: resolve_l3 miss sends a request and returns false") {
    MockEth   dev;
    ArpModule arp;
    NetStack  stack;
    ASSERT_TRUE(stack.attach(dev, InDevice{dev.my_mac, ip(10, 0, 2, 15), ip(10, 0, 2, 2)}));

    EthAddr out{};
    ASSERT_FALSE(arp.resolve_l3(dev, ip(10, 0, 2, 2), stack, out));  // miss -> async
    ASSERT_EQ(arp.request_count(), 1u);
    ASSERT_EQ(dev.sent.size(), 1u);
    ArpPacket req;
    parse_arp(dev.sent[0].bytes.data(), req);
    ASSERT_EQ(req.oper, kArpRequest);
    ASSERT_EQ(req.tpa, ip(10, 0, 2, 2));
}

TEST("arp_module: resolve_l3 hit after the cache is populated") {
    MockEth   dev;
    ArpModule arp;
    NetStack  stack;
    ASSERT_TRUE(stack.attach(dev, InDevice{dev.my_mac, ip(10, 0, 2, 15), ip(10, 0, 2, 2)}));
    // Populate by inserting through a reply, then resolve_l3 must hit.
    ArpPacket p{};
    p.htype = 1;
    p.ptype = 0x0800;
    p.hlen  = 6;
    p.plen  = 4;
    p.oper  = kArpReply;
    p.sha   = mac(0xC0, 0xFF, 0xEE, 0x00, 0x00, 0x01);
    p.spa   = ip(10, 0, 2, 2);
    p.tha   = dev.my_mac;
    p.tpa   = ip(10, 0, 2, 15);
    std::vector<uint8_t> f(14 + 28, 0);
    f[12] = 0x08;
    f[13] = 0x06;
    build_arp(p, f.data() + 14);
    dev.rx.push_back(std::move(f));
    stack.add_protocol(kEtherTypeArp, arp);
    stack.poll();

    EthAddr out{};
    ASSERT_TRUE(arp.resolve_l3(dev, ip(10, 0, 2, 2), stack, out));  // hit -- no new request
    ASSERT_EQ(out, mac(0xC0, 0xFF, 0xEE, 0x00, 0x00, 0x01));
    ASSERT_EQ(arp.request_count(), 0u);  // resolved from cache, no request sent
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
