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

/// Heap buffer RAII guard. Local struct (no <memory>) -- the freestanding
/// header gate (scripts/check_freestanding_headers.py) rejects <memory>.
/// new[]/delete[] route to the kernel crt_stub operator new/kmalloc, which is
/// fine on the freestanding path.
struct HeapBuf {
    uint8_t* p;
    explicit HeapBuf(size_t n) : p(new uint8_t[n]) {}
    ~HeapBuf() { delete[] p; }
    HeapBuf(const HeapBuf&)            = delete;
    HeapBuf& operator=(const HeapBuf&) = delete;
};
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
        // Heap-allocated: kMaxIcmp (1484B) > 1024B frame budget. Network path
        // avoids static to dodge SMP-shared-buffer hazards. Fully rewritten
        // below (loop writes all n bytes, then checksum bytes) -> no zero-init
        // needed.
        HeapBuf buf(kMaxIcmp);
        for (uint32_t i = 0; i < n; ++i) {
            buf.p[i] = payload.data()[i];  // copy header + echo data verbatim
        }
        buf.p[0]          = kIcmpEchoReply;  // type 0
        buf.p[1]          = 0;               // code 0
        buf.p[2]          = 0;
        buf.p[3]          = 0;  // zero checksum before recompute
        const uint16_t cs = cinux::lib::internet_checksum(buf.p, n);
        buf.p[2]          = static_cast<uint8_t>(cs >> 8);
        buf.p[3]          = static_cast<uint8_t>(cs & 0xFF);
        // Reply to the request's source; Ipv4Module sources our local address.
        static_cast<void>(ipv4.send(dev, ip.src, kIpProtoIcmp, buf.p, n, stack));
    } else if (hdr.type == kIcmpEchoReply) {
        ++reply_count_;
        last_id_  = hdr.id;
        last_seq_ = hdr.seq;
        // SOCK_RAW ping: deliver a COPY of the whole ICMP message (header +
        // data) to every registered RawSocket so a busybox recvfrom() reads it.
        // payload is borrowed (the device recycles the frame after dispatch);
        // RawSocket::on_icmp_reply copies under its own lock.  No lock here --
        // the list is mutated only at socket construct/destruct (syscall path),
        // and handle() runs single-threaded per device poll.
        for (uint32_t i = 0; i < kMaxRawSockets; ++i) {
            if (raw_sockets_[i] != nullptr) {
                raw_sockets_[i]->on_icmp_reply(ip, payload);
            }
        }
    }
}

void IcmpModule::register_raw_socket(RawListener* s) {
    if (s == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < kMaxRawSockets; ++i) {
        if (raw_sockets_[i] == s) {
            return;  // already registered (idempotent)
        }
    }
    for (uint32_t i = 0; i < kMaxRawSockets; ++i) {
        if (raw_sockets_[i] == nullptr) {
            raw_sockets_[i] = s;
            return;
        }
    }
    // table full -- ignore (kMaxRawSockets covers the realistic ping load)
}

void IcmpModule::unregister_raw_socket(RawListener* s) {
    if (s == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < kMaxRawSockets; ++i) {
        if (raw_sockets_[i] == s) {
            raw_sockets_[i] = nullptr;
            return;  // a socket registers at most once
        }
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
