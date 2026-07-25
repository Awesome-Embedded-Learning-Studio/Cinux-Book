/**
 * @file kernel/net/udp.cpp
 * @brief UdpModule -- inbound checksum verify + port demux, outbound TX.
 *
 * Inbound: parse the 8-byte header, verify the pseudo-header checksum (unless
 * the sender set checksum=0, RFC 768 "no checksum"), then dispatch to the
 * listener bound to the destination port.  Outbound: lay out [pseudo-header |
 * UDP header | payload] in one buffer, run internet_checksum once over the whole
 * run, embed it, and emit the UDP portion via Ipv4Module::send (proto=17).
 * Zero kprintf.
 *
 * Checksum note: the UDP checksum covers a 12-byte pseudo-header (src IP, dst IP,
 * zero, proto, UDP length) + the UDP header + payload.  Computing it over one
 * contiguous buffer (rather than summing a pseudo-header partial + L4 bytes
 * separately) avoids the fold/complement plumbing -- internet_checksum does both
 * in one pass, and verify_internet_checksum re-sums the same run on RX.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/udp.hpp"

#include <cinux/checksum.hpp>
#include <cstring>

#include "kernel/net/net_stack.hpp"  // InDevice, NetStack

namespace cinux::net {

namespace {

/// Max UDP payload we will send: an L4 datagram behind a 14-byte L2 + 20-byte
/// IPv4 header must fit one Ethernet frame.  The checksum buffer stays off the
/// kernel stack (HeapBuf below).
constexpr uint32_t kMaxUdpPayload = 1518 - 14 - sizeof(Ipv4Header) - sizeof(UdpHeader);

/// Local RAII heap buffer (no <memory>): the TX/RX checksum buffers (up to
/// ~1.5KB) stay off the kernel stack.  See ipv4.cpp's HeapBuf for the
/// freestanding-header rationale (new[]/delete[] via crt_stub, not <memory>).
struct HeapBuf {
    uint8_t* p;
    explicit HeapBuf(size_t n) : p(new uint8_t[n]) {}
    ~HeapBuf() { delete[] p; }
    HeapBuf(const HeapBuf&)            = delete;
    HeapBuf& operator=(const HeapBuf&) = delete;
};

/// @brief Write the 12-byte UDP pseudo-header at @p ph.  @p udp_len is the UDP
///        length field (header + payload); TX and RX reconstruct it identically
///        so the receiver's checksum verify matches the sender's compute.
void build_pseudo_header(uint8_t* ph, Ipv4Addr src, Ipv4Addr dst, uint16_t udp_len) {
    for (int i = 0; i < 4; ++i) {
        ph[i]     = src.oct[i];
        ph[4 + i] = dst.oct[i];
    }
    ph[8]  = 0;  // zero
    ph[9]  = kIpProtoUdp;
    ph[10] = static_cast<uint8_t>(udp_len >> 8);
    ph[11] = static_cast<uint8_t>(udp_len & 0xFF);
}

}  // namespace

bool UdpModule::bind(uint16_t port, UdpListener& listener) {
    for (uint32_t i = 0; i < kMaxUdpPorts; ++i) {
        if (ports_[i].l != nullptr && ports_[i].port == port) {
            return false;  // already bound
        }
    }
    for (uint32_t i = 0; i < kMaxUdpPorts; ++i) {
        if (ports_[i].l == nullptr) {
            ports_[i].port = port;
            ports_[i].l    = &listener;
            return true;
        }
    }
    return false;  // table full
}

void UdpModule::unbind(uint16_t port) {
    for (uint32_t i = 0; i < kMaxUdpPorts; ++i) {
        if (ports_[i].l != nullptr && ports_[i].port == port) {
            ports_[i].l    = nullptr;
            ports_[i].port = 0;
            return;
        }
    }
}

cinux::lib::ErrorOr<void> UdpModule::send(NetDevice& dev, Ipv4Addr dst, uint16_t src_port,
                                          uint16_t dst_port, const uint8_t* data, uint32_t len,
                                          Ipv4Module& ipv4, NetStack& stack) {
    if (len > kMaxUdpPayload) {
        return cinux::lib::Error::InvalidArgument;
    }
    const InDevice* cfg = stack.config_for(dev);
    if (cfg == nullptr) {
        return cinux::lib::Error::InvalidArgument;  // no source address for this device
    }

    const uint16_t udp_len = static_cast<uint16_t>(sizeof(UdpHeader) + len);

    // One contiguous run = [pseudo-header(12) | UDP header(8) | payload(len)].
    // internet_checksum over the whole run yields the UDP checksum; we then send
    // the UDP portion (past the pseudo-header) so the pseudo-header never goes on
    // the wire.  The checksum field is zero during compute, embedded after.
    HeapBuf buf(12 + sizeof(UdpHeader) + len);
    build_pseudo_header(buf.p, cfg->local, dst, udp_len);

    UdpHeader h{};
    h.src_port = src_port;
    h.dst_port = dst_port;
    h.length   = udp_len;
    h.checksum = 0;
    build_udp_header(h, buf.p + 12);
    if (len > 0) {
        std::memcpy(buf.p + 12 + sizeof(UdpHeader), data, len);
    }

    uint16_t cs = cinux::lib::internet_checksum(buf.p, 12 + sizeof(UdpHeader) + len);
    // RFC 768: a computed checksum of 0 is transmitted as 0xFFFF (0 == "no checksum").
    if (cs == 0) {
        cs = 0xFFFF;
    }
    buf.p[12 + 6] = static_cast<uint8_t>(cs >> 8);  // UDP header checksum field
    buf.p[12 + 7] = static_cast<uint8_t>(cs & 0xFF);

    // Emit the UDP portion (past the 12-byte pseudo-header).
    return ipv4.send(dev, dst, kIpProtoUdp, buf.p + 12, sizeof(UdpHeader) + len, stack);
}

void UdpModule::handle(const Ipv4Header& ip, FrameView payload, NetDevice& /*dev*/,
                       Ipv4Module& /*ipv4*/, NetStack& /*stack*/) {
    if (payload.size() < sizeof(UdpHeader)) {
        return;  // short / not a UDP datagram
    }
    UdpHeader h;
    parse_udp(payload.data(), h);

    // h.length must cover at least the header and not exceed what was delivered.
    if (h.length < sizeof(UdpHeader) || h.length > payload.size()) {
        return;  // insane length -> drop
    }

    // Verify the checksum unless the sender set checksum=0 ("no checksum", RFC 768).
    // Reconstruct the SAME run the sender summed: pseudo-header(12, UDP length =
    // h.length) + the first h.length received bytes (UDP header + payload, with
    // the embedded checksum).  Trailing bytes beyond h.length are padding, not
    // summed.
    if (h.checksum != 0) {
        HeapBuf buf(12 + h.length);
        build_pseudo_header(buf.p, ip.src, ip.dst, h.length);
        std::memcpy(buf.p + 12, payload.data(), h.length);
        if (!cinux::lib::verify_internet_checksum(buf.p, 12 + h.length)) {
            return;  // corrupt -> silently drop
        }
    }

    const uint32_t plen = h.length - sizeof(UdpHeader);
    for (uint32_t i = 0; i < kMaxUdpPorts; ++i) {
        if (ports_[i].l != nullptr && ports_[i].port == h.dst_port) {
            ports_[i].l->on_udp(ip, h.src_port,
                                FrameView(payload.data() + sizeof(UdpHeader), plen));
            return;
        }
    }
    // no listener bound to dst_port -> silent drop (like Linux's no-socket-on-port)
}

}  // namespace cinux::net
