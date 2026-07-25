/**
 * @file kernel/net/ipv4.cpp
 * @brief Ipv4Module -- inbound validation/dispatch + outbound IP TX.
 *
 * Inbound: parse, sanity-check (version / IHL / total_len / header checksum via
 * verify_internet_checksum), dispatch the L4 payload by IP proto via the inner
 * L4Handler table (proto->handler, like Linux inet_protos).  No fragmentation /
 * options today (IHL=5 only); a non-5 IHL is honoured for the header-length
 * computation but options bytes are not parsed.  Zero kprintf.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/ipv4.hpp"

#include <cinux/checksum.hpp>

#include "kernel/net/arp.hpp"        // ArpModule
#include "kernel/net/icmp.hpp"       // IcmpModule
#include "kernel/net/net_stack.hpp"  // InDevice, NetStack

namespace cinux::net {

namespace {

// Local RAII heap buffer (no <memory>): owns a new[]/delete[] byte slice so the
// ~1504B IP TX buffer stays off the stack. new/delete route through the
// freestanding crt_stub operator new / kmalloc, which the CI freestanding
// header gate permits (unlike the hosted <memory> header).
struct HeapBuf {
    uint8_t* p;
    explicit HeapBuf(size_t n) : p(new uint8_t[n]) {}
    ~HeapBuf() { delete[] p; }
    HeapBuf(const HeapBuf&)            = delete;
    HeapBuf& operator=(const HeapBuf&) = delete;
};

}  // namespace

Ipv4Module::Ipv4Module(IcmpModule& icmp, ArpModule* arp) : arp_(arp) {
    // ICMP (proto 1) routes through the L4 table like any other L4 protocol --
    // registered here so existing call sites (which only pass icmp + arp) are
    // unchanged.  icmp.hpp is included above, so the IcmpModule->L4Handler
    // conversion is visible (the header forward-declares IcmpModule only).
    add_l4(kIpProtoIcmp, icmp);
}

void Ipv4Module::on_frame(const L2Info& /*l2*/, FrameView payload, NetDevice& dev,
                          NetStack& stack) {
    if (payload.size() < sizeof(Ipv4Header)) {
        return;  // short / not an IPv4 packet
    }
    Ipv4Header ip;
    parse_ipv4(payload.data(), ip);

    if (ipv4_version(ip) != 4) {
        return;
    }
    const uint8_t ihl = ipv4_ihl(ip);
    if (ihl < 5) {
        return;  // impossibly short header
    }
    const uint32_t header_len = static_cast<uint32_t>(ihl) * 4;
    if (header_len > payload.size()) {
        return;  // truncated header
    }

    // Header checksum: re-summing a valid header (with embedded checksum) == 0xFFFF.
    if (!cinux::lib::verify_internet_checksum(payload.data(), header_len)) {
        return;  // corrupt -> silently drop
    }

    // Use total_len if sane, else the bytes we actually have.
    uint32_t total = ip.total_len;
    if (total < header_len || total > payload.size()) {
        total = payload.size();  // lenient: trust what the device delivered
    }
    const uint32_t l4_len = total - header_len;
    const uint8_t* l4     = payload.data() + header_len;

    // Dispatch by IP protocol number via the inner L4 table (proto->handler,
    // like Linux inet_protos): ICMP=1, UDP=17, ...  No match -> silent drop.
    for (uint32_t i = 0; i < kMaxL4; ++i) {
        if (l4_[i].h != nullptr && l4_[i].proto == ip.proto) {
            l4_[i].h->handle(ip, FrameView(l4, l4_len), dev, *this, stack);
            break;
        }
    }
}

cinux::lib::ErrorOr<void> Ipv4Module::send(NetDevice& dev, Ipv4Addr dst, uint8_t proto,
                                           const uint8_t* l4, uint32_t len, NetStack& stack) {
    static constexpr uint32_t kBuf = 1518 - 14;  // max IP packet behind a 14-byte L2 header
    if (len > kBuf - sizeof(Ipv4Header)) {
        return cinux::lib::Error::InvalidArgument;
    }
    const InDevice* cfg = stack.config_for(dev);
    if (cfg == nullptr) {
        return cinux::lib::Error::InvalidArgument;  // no source address for this device
    }

    Ipv4Header ip{};
    ip.ver_ihl    = static_cast<uint8_t>((4 << 4) | kIpv4HdrWords);
    ip.total_len  = static_cast<uint16_t>(sizeof(Ipv4Header) + len);
    ip.id         = next_id_++;
    ip.flags_frag = 0;  // no fragmentation (a ping fits one packet)
    ip.ttl        = kIpv4TtlDefault;
    ip.proto      = proto;
    ip.checksum   = 0;  // computed below
    ip.src        = cfg->local;
    ip.dst        = dst;

    // Heap-allocated: kBuf (1504B) > 1024B frame budget. Network path uses the
    // heap (not a static buffer) so concurrent TX on SMP does not share storage.
    HeapBuf  buf(kBuf);
    uint8_t* p = buf.p;
    build_ipv4_header(ip, p);  // checksum field still 0
    for (uint32_t i = 0; i < len; ++i) {
        p[sizeof(Ipv4Header) + i] = l4[i];
    }
    // Header checksum over the 20-byte header (checksum field zeroed).
    const uint16_t cs = cinux::lib::internet_checksum(p, sizeof(Ipv4Header));
    p[10]             = static_cast<uint8_t>(cs >> 8);
    p[11]             = static_cast<uint8_t>(cs & 0xFF);

    // Next hop: Ethernet resolves dst via ARP (async); loopback skips L2.
    EthAddr next_hop{};
    if (dev.has_ethernet_header() && arp_ != nullptr) {
        if (!arp_->resolve_l3(dev, dst, stack, next_hop)) {
            return {};  // ARP request sent; IP packet deferred to the next poll
        }
    }
    return stack.send_l3(dev, next_hop, kEtherTypeIpv4, p, sizeof(Ipv4Header) + len);
}

}  // namespace cinux::net
