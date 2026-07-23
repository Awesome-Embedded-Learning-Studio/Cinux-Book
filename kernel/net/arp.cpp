/**
 * @file kernel/net/arp.cpp
 * @brief ArpModule -- ARP ethertype handler + L3 next-hop resolver.
 *
 * On an inbound ARP frame: learn the sender (request OR reply) into the cache,
 * and answer requests for OUR local IP on the device they arrived on (FOLD-B:
 * the reply egresses the SAME NIC).  resolve_l3() serves a cached MAC or fires
 * an async ARP request (retried on the next poll -- same sti/hlt patience as RX,
 * never blocks).  Zero kprintf (pure logic; host-linkable).
 *
 * Namespace: cinux::net
 */

#include "kernel/net/arp.hpp"

#include "kernel/net/net_stack.hpp"  // InDevice, NetStack (full definitions)

namespace cinux::net {

namespace {

/// Build a 28-byte ARP body (caller passes it to stack.send_l3; the device
/// composes the Ethernet header -- ARP module never touches L2).
void build_arp_body(uint16_t oper, EthAddr sha, Ipv4Addr spa, EthAddr tha, Ipv4Addr tpa,
                    uint8_t* out) {
    ArpPacket p{};
    p.htype = kArpHtypeEth;
    p.ptype = kArpPtypeIpv4;
    p.hlen  = 6;
    p.plen  = 4;
    p.oper  = oper;
    p.sha   = sha;
    p.spa   = spa;
    p.tha   = tha;
    p.tpa   = tpa;
    build_arp(p, out);
}

EthAddr broadcast_mac() {
    EthAddr b{};
    for (int i = 0; i < 6; ++i) {
        b.oct[i] = 0xFF;
    }
    return b;
}

}  // namespace

void ArpModule::on_frame(const L2Info& /*l2*/, FrameView payload, NetDevice& dev, NetStack& stack) {
    if (payload.size() < sizeof(ArpPacket)) {
        return;  // short / malformed -- drop
    }
    ArpPacket arp;
    parse_arp(payload.data(), arp);

    // Learn the sender regardless of op (a request also tells us spa->sha).
    cache_.insert(arp.spa, arp.sha);

    // Answer requests for OUR local IP on the device the request arrived on.
    if (arp.oper != kArpRequest) {
        return;
    }
    const InDevice* cfg = stack.config_for(dev);
    if (cfg == nullptr || !(arp.tpa == cfg->local)) {
        return;  // not for us (or no config) -> ignore
    }
    EthAddr our_mac;
    if (!dev.mac(our_mac)) {
        return;  // no-L2 device has no MAC to advertise
    }
    uint8_t body[28];
    build_arp_body(kArpReply, our_mac, cfg->local, arp.sha, arp.spa, body);
    // Reply unicasts to the requester's MAC; egresses the SAME device (FOLD-B).
    (void)stack.send_l3(dev, arp.sha, kEtherTypeArp, body, sizeof(body));
}

bool ArpModule::resolve_l3(NetDevice& dev, Ipv4Addr ip, NetStack& stack, EthAddr& out) {
    if (cache_.lookup(ip, out)) {
        return true;  // resolved
    }
    // Miss: send an ARP request (async -- retried on the next poll, never blocks).
    const InDevice* cfg = stack.config_for(dev);
    if (cfg == nullptr) {
        return false;
    }
    EthAddr our_mac;
    if (!dev.mac(our_mac)) {
        return false;  // no-L2 device -> ARP is meaningless
    }
    uint8_t body[28];
    build_arp_body(kArpRequest, our_mac, cfg->local, EthAddr{}, ip, body);
    (void)stack.send_l3(dev, broadcast_mac(), kEtherTypeArp, body, sizeof(body));
    ++sent_requests_;
    return false;  // not yet resolved -- caller retries on the next poll
}

}  // namespace cinux::net
