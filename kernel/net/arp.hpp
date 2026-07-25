/**
 * @file kernel/net/arp.hpp
 * @brief ARP wire layout (Ethernet/IPv4) + a fixed ARP cache.
 *
 * Host-order parsed packet struct (see net_types.hpp byte-order policy: multi-
 * byte fields reconstructed via hi/lo bytes at parse time; addresses memcpy'd
 * verbatim).  parse_arp/build_arp convert between wire bytes and the struct.
 *
 * ArpCache is a small fixed-slot IP->MAC table (round-robin eviction) -- the
 * data structure ArpModule (L1) consults.  Header-only so host unit tests link
 * it with zero kernel sources.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cstdint>

#include "kernel/net/net_types.hpp"
#include "kernel/net/protocol_handler.hpp"  // ProtocolHandler (ArpModule base)

namespace cinux::net {

/// @brief ARP packet (Ethernet/IPv4), HOST-order parsed view.  28 bytes on wire.
struct ArpPacket {
    uint16_t htype;  ///< hardware type (1 = Ethernet)
    uint16_t ptype;  ///< protocol type (0x0800 = IPv4)
    uint8_t  hlen;   ///< hardware addr len (6)
    uint8_t  plen;   ///< protocol addr len (4)
    uint16_t oper;   ///< operation (1 = request, 2 = reply)
    EthAddr  sha;    ///< sender hardware addr
    Ipv4Addr spa;    ///< sender protocol addr
    EthAddr  tha;    ///< target hardware addr
    Ipv4Addr tpa;    ///< target protocol addr
};
static_assert(sizeof(ArpPacket) == 28, "ARP Ethernet/IPv4 packet is 28 bytes");

constexpr uint16_t kArpHtypeEth  = 1;       ///< ARP hardware type: Ethernet
constexpr uint16_t kArpPtypeIpv4 = 0x0800;  ///< ARP protocol type: IPv4
constexpr uint16_t kArpRequest   = 1;       ///< ARP operation: request
constexpr uint16_t kArpReply     = 2;       ///< ARP operation: reply

/// @brief Parse 28 wire bytes into an ArpPacket (host order).
inline void parse_arp(const uint8_t* p, ArpPacket& out) {
    out.htype = (static_cast<uint16_t>(p[0]) << 8) | p[1];
    out.ptype = (static_cast<uint16_t>(p[2]) << 8) | p[3];
    out.hlen  = p[4];
    out.plen  = p[5];
    out.oper  = (static_cast<uint16_t>(p[6]) << 8) | p[7];
    for (int i = 0; i < 6; ++i) {
        out.sha.oct[i] = p[8 + i];
    }
    for (int i = 0; i < 4; ++i) {
        out.spa.oct[i] = p[14 + i];
    }
    for (int i = 0; i < 6; ++i) {
        out.tha.oct[i] = p[18 + i];
    }
    for (int i = 0; i < 4; ++i) {
        out.tpa.oct[i] = p[24 + i];
    }
}

/// @brief Serialise an ArpPacket into 28 wire bytes (big-endian multi-byte).
inline void build_arp(const ArpPacket& in, uint8_t* p) {
    p[0] = static_cast<uint8_t>(in.htype >> 8);
    p[1] = static_cast<uint8_t>(in.htype & 0xFF);
    p[2] = static_cast<uint8_t>(in.ptype >> 8);
    p[3] = static_cast<uint8_t>(in.ptype & 0xFF);
    p[4] = in.hlen;
    p[5] = in.plen;
    p[6] = static_cast<uint8_t>(in.oper >> 8);
    p[7] = static_cast<uint8_t>(in.oper & 0xFF);
    for (int i = 0; i < 6; ++i) {
        p[8 + i]  = in.sha.oct[i];
        p[18 + i] = in.tha.oct[i];
    }
    for (int i = 0; i < 4; ++i) {
        p[14 + i] = in.spa.oct[i];
        p[24 + i] = in.tpa.oct[i];
    }
}

/// @brief Fixed-slot IP->MAC cache (round-robin eviction when full).
///
/// The data structure ArpModule consults to source a next-hop MAC before an
/// IPv4 TX, and that it populates from observed ARP replies/requests.  Kept
/// tiny (8 slots -- one gateway + a few peers is plenty for ping); a future
/// high-throughput stack would age entries (no ageing today, intentional).
class ArpCache {
public:
    static constexpr uint32_t kSlots = 8;

    /// @brief Lookup @p ip.  Writes the MAC into @p out and returns true on hit.
    bool lookup(Ipv4Addr ip, EthAddr& out) const {
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (entries_[i].used && entries_[i].ip == ip) {
                out = entries_[i].mac;
                return true;
            }
        }
        return false;
    }

    /// @brief Insert/update @p ip -> @p mac.  Evicts the slot at the round-robin
    ///        cursor when full (no LRU -- ping-scale: few peers, no churn).
    void insert(Ipv4Addr ip, EthAddr mac) {
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (entries_[i].used && entries_[i].ip == ip) {
                entries_[i].mac = mac;  // refresh existing
                return;
            }
        }
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (!entries_[i].used) {
                entries_[i].used = true;
                entries_[i].ip   = ip;
                entries_[i].mac  = mac;
                return;
            }
        }
        entries_[next_].used = true;
        entries_[next_].ip   = ip;
        entries_[next_].mac  = mac;
        next_                = (next_ + 1) % kSlots;
    }

    /// @brief Reset the cache (test isolation).
    void clear() {
        for (uint32_t i = 0; i < kSlots; ++i) {
            entries_[i].used = false;
        }
        next_ = 0;
    }

private:
    struct Entry {
        bool     used;
        Ipv4Addr ip;
        EthAddr  mac;
    };
    Entry    entries_[kSlots]{};
    uint32_t next_ = 0;  ///< round-robin eviction cursor
};

/// @brief ARP protocol module (ethertype 0x0806 handler) + the L3 next-hop
///        resolver IPv4 consults before a TX.
///
/// On an inbound ARP frame: learn the sender (request or reply) into the cache,
/// and answer requests for OUR local IP on the device they arrived on (FOLD-B:
/// the reply egresses the SAME NIC).  resolve_l3() serves a cached MAC or fires
/// an async ARP request (retried on the next poll -- same patience as RX).
class ArpModule : public ProtocolHandler {
public:
    /// @brief Handle an inbound ARP frame (request -> reply; reply/request -> cache).
    void on_frame(const L2Info& l2, FrameView payload, NetDevice& dev, NetStack& stack) override;

    /// @brief Cached-MAC lookup without sending (host-testable).
    bool lookup(Ipv4Addr ip, EthAddr& out) const { return cache_.lookup(ip, out); }

    /// @brief Resolve @p ip on @p dev.  Cache hit -> fill @p out, return true.
    ///        Miss -> send an ARP request (async; retried next poll), return false.
    ///        No-op on a no-L2 device (loopback) -- returns false without sending.
    bool resolve_l3(NetDevice& dev, Ipv4Addr ip, NetStack& stack, EthAddr& out);

    /// @brief Reset state (test isolation).
    void reset() {
        cache_.clear();
        sent_requests_ = 0;
    }
    uint32_t request_count() const { return sent_requests_; }

private:
    ArpCache cache_;
    uint32_t sent_requests_ = 0;
};

}  // namespace cinux::net
