/**
 * @file kernel/net/net_types.hpp
 * @brief POD wire/parsed types + constants for the L3 network stack.
 *
 * The bottom of the net stack: value types shared by every layer (Ethernet,
 * ARP, IPv4, ICMP) and by the NetDevice/ProtocolHandler seams.  ZERO logic,
 * ZERO device dependencies -- nothing here names E1000Controller, DmaBuffer, or
 * any arch/irq header (the four decoupling greps hold from line one).
 *
 * Byte-order policy (deliberate, to kill the classic "green test, broken wire"
 * trap): multi-byte integers are parsed into HOST order at the seam
 * (parse_eth / ArpPacket helpers reconstruct via (hi<<8)|lo), and constants are
 * compared in host order.  Addresses are 4/6-octet byte arrays memcpy'd verbatim
 * (order-agnostic), so the data path never byte-swaps a field.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/span.hpp>
#include <cstdint>

namespace cinux::net {

/// Ethernet address.  Value type; the literal 6 lives here and nowhere else --
/// ARP/IPv4 take EthAddr, never a raw[6].  (FOLD-A: a device with no L2 address
/// never produces one of these -- see NetDevice::mac's bool return.)
struct EthAddr {
    uint8_t oct[6];
};

/// @brief Ethernet address equality (ARP-cache lookup key component).
inline bool operator==(const EthAddr& a, const EthAddr& b) {
    for (int i = 0; i < 6; ++i) {
        if (a.oct[i] != b.oct[i]) {
            return false;
        }
    }
    return true;
}

/// @brief Ethernet II header, HOST-order parsed view.
///
/// @note @p ethertype is HOST order (reconstructed from wire hi/lo bytes at
///       parse time), so it compares directly against kEtherType*.  This struct
///       is NOT a wire overlay -- do not memcpy 14 raw frame bytes into it
///       (that would put the big-endian ethertype field in the wrong host
///       representation on a little-endian CPU).  Use parse_eth().
struct EthHdr {
    EthAddr  dst;
    EthAddr  src;
    uint16_t ethertype;  ///< HOST order (e.g. kEtherTypeIpv4 == 0x0800)
};
static_assert(sizeof(EthHdr) == 14, "Ethernet II header is 14 bytes");

/// @brief Reconstruct an Ethernet header from raw frame bytes (host order).
/// @param p   pointer to >=14 bytes of frame (dst, src, ethertype).
/// @param out filled with dst/src (verbatim) + ethertype (hi<<8 | lo).
inline void parse_eth(const uint8_t* p, EthHdr& out) {
    for (int i = 0; i < 6; ++i) {
        out.dst.oct[i] = p[i];
        out.src.oct[i] = p[6 + i];
    }
    out.ethertype = (static_cast<uint16_t>(p[12]) << 8) | p[13];
}

constexpr uint16_t kEtherTypeArp  = 0x0806;  ///< Ethernet -> ARP
constexpr uint16_t kEtherTypeIpv4 = 0x0800;  ///< Ethernet -> IPv4

/// @brief Non-owning view over a frame/payload (L3 bytes handed to a handler).
///
/// Backed by the device's buffer (zero-copy RX) or a device-internal scratch
/// (copy RX).  Valid ONLY for the duration of the ProtocolHandler::on_frame
/// call that receives it -- a handler that needs the bytes longer MUST copy
/// them (borrow, never retain).  Mirrors Linux sk_buff data/len WITHOUT
/// refcount/clone/headroom (proportionate to ping).
using FrameView = cinux::lib::ConstByteSpan;

/// @brief IPv4 address, 4 octets memcpy'd verbatim (no endian swap on data path).
struct Ipv4Addr {
    uint8_t oct[4];
};

/// @brief IPv4 address equality (ARP-cache key, route match).
inline bool operator==(const Ipv4Addr& a, const Ipv4Addr& b) {
    for (int i = 0; i < 4; ++i) {
        if (a.oct[i] != b.oct[i]) {
            return false;
        }
    }
    return true;
}

/// QEMU user-net (SLIRP) static config: guest 10.0.2.15, gateway 10.0.2.2 (the
/// SLIRP host answers ARP + ICMP echo -- the F7 integration target).
constexpr Ipv4Addr kSlirpGuest   = {{10, 0, 2, 15}};
constexpr Ipv4Addr kSlirpGateway = {{10, 0, 2, 2}};
/// IPv4 loopback (127.0.0.1) -- the deterministic L3 testbed (no SLIRP timing).
constexpr Ipv4Addr kLoopbackAddr = {{127, 0, 0, 1}};

/// @brief L2 metadata bundle handed to a protocol handler alongside the payload.
///
/// For an Ethernet device (has_ethernet_header()==true): src/dst MAC + parsed
/// ethertype.  For a no-L2 device (loopback): has_ethernet==false, src/dst are
/// zero-filled, ethertype is derived (from the IPv4 version nibble today).  The
/// handler replies via stack.send_l3(dev, ...) -- it does NOT need to re-parse.
struct L2Info {
    bool     has_ethernet = true;  ///< false for loopback / raw-L3 devices
    EthAddr  src{};                ///< sender MAC (zero if no L2)
    EthAddr  dst{};                ///< destination MAC (zero if no L2)
    uint16_t ethertype = 0;        ///< HOST order (kEtherType*)
};

}  // namespace cinux::net
