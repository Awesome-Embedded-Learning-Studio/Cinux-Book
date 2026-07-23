/**
 * @file kernel/net/tcp.cpp
 * @brief TcpModule -- checksum gate + 3-way handshake FSM (batches 1-2).
 *
 * handle() verifies the TCP pseudo-header checksum (proto 6, mandatory -- unlike
 * UDP a 0 checksum is NOT "no checksum"), then advances the connection state
 * machine for the matching 4-tuple: passive-open a SYN to a listened port
 * ( SYN -> SYN-ACK ), complete an active open on the SYN-ACK ( -> ACK ), and
 * reach ESTABLISHED on the 3rd ACK.  An unroutable SYN is answered with RST;
 * an inbound RST tears down any connection.  Data + 4-way teardown land in
 * batch 3.  Zero kprintf.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/tcp.hpp"

#include <cinux/checksum.hpp>
#include <cstring>

#include "kernel/net/net_stack.hpp"  // InDevice, NetStack

namespace cinux::net {

namespace {

/// A fixed window every segment advertises -- no flow control this milestone
/// (sliding window / retransmit are follow-up).  Nonzero so a peer has room.
constexpr uint16_t kTcpWindow = 8192;

/// Local RAII heap buffer (no <memory>): the TX/RX checksum buffers (up to
/// ~1.5KB) stay off the kernel stack.  See ipv4.cpp's HeapBuf for the
/// freestanding rationale (new[]/delete[] via crt_stub, not <memory>).
struct HeapBuf {
    uint8_t* p;
    explicit HeapBuf(size_t n) : p(new uint8_t[n]) {}
    ~HeapBuf() { delete[] p; }
    HeapBuf(const HeapBuf&)            = delete;
    HeapBuf& operator=(const HeapBuf&) = delete;
};

/// @brief Write the 12-byte TCP pseudo-header at @p ph (proto 6).  @p tcp_len is
///        the TCP segment length (header + data); the receiver sums the SAME run
///        the sender did, so verify matches compute.  Identical to UDP's, bar
///        the protocol byte.
void build_pseudo_header(uint8_t* ph, Ipv4Addr src, Ipv4Addr dst, uint16_t tcp_len) {
    for (int i = 0; i < 4; ++i) {
        ph[i]     = src.oct[i];
        ph[4 + i] = dst.oct[i];
    }
    ph[8]  = 0;  // zero
    ph[9]  = kIpProtoTcp;
    ph[10] = static_cast<uint8_t>(tcp_len >> 8);
    ph[11] = static_cast<uint8_t>(tcp_len & 0xFF);
}

}  // namespace

// ============================================================
// Connection table / listeners
// ============================================================

bool TcpModule::listen(uint16_t local_port, TcpListener& listener) {
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        if (listens_[i].l != nullptr && listens_[i].port == local_port) {
            return false;  // already listened
        }
    }
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        if (listens_[i].l == nullptr) {
            listens_[i].port = local_port;
            listens_[i].l    = &listener;
            return true;
        }
    }
    return false;  // table full
}

void TcpModule::stop_listen(uint16_t local_port) {
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        if (listens_[i].l != nullptr && listens_[i].port == local_port) {
            listens_[i].l    = nullptr;
            listens_[i].port = 0;
            return;
        }
    }
}

TcpListener* TcpModule::find_listener(uint16_t port) {
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        if (listens_[i].l != nullptr && listens_[i].port == port) {
            return listens_[i].l;
        }
    }
    return nullptr;
}

TcpModule::Connection* TcpModule::find(uint16_t local_port, Ipv4Addr remote_addr,
                                       uint16_t remote_port) {
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        Connection& c = cons_[i];
        if (c.state != TcpState::kClosed && c.local_port == local_port &&
            c.remote_addr == remote_addr && c.remote_port == remote_port) {
            return &c;
        }
    }
    return nullptr;
}

TcpModule::Connection* TcpModule::alloc(uint16_t local_port, Ipv4Addr remote_addr,
                                        uint16_t remote_port, TcpListener* listener) {
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        if (cons_[i].state == TcpState::kClosed) {
            Connection& c = cons_[i];
            c.state       = TcpState::kSynSent;  // occupied; caller refines
            c.local_port  = local_port;
            c.remote_addr = remote_addr;
            c.remote_port = remote_port;
            c.iss         = 0;
            c.snd_nxt     = 0;
            c.rcv_nxt     = 0;
            c.listener    = listener;
            return &c;
        }
    }
    return nullptr;  // table full
}

uint32_t TcpModule::next_isn() {
    const uint32_t v = isn_counter_;
    isn_counter_ += 0x00010000;  // 64K stride: connection seq-spaces never overlap in tests
    return v;
}

uint32_t TcpModule::connection_count() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        if (cons_[i].state != TcpState::kClosed) {
            ++n;
        }
    }
    return n;
}

TcpState TcpModule::state_of(uint16_t local_port, Ipv4Addr remote_addr,
                             uint16_t remote_port) const {
    for (uint32_t i = 0; i < kMaxTcpCons; ++i) {
        const Connection& c = cons_[i];
        if (c.state != TcpState::kClosed && c.local_port == local_port &&
            c.remote_addr == remote_addr && c.remote_port == remote_port) {
            return c.state;
        }
    }
    return TcpState::kClosed;
}

// ============================================================
// TX primitive
// ============================================================

cinux::lib::ErrorOr<void> TcpModule::send_segment(NetDevice& dev, Ipv4Addr dst, uint16_t src_port,
                                                  uint16_t dst_port, uint32_t seq, uint32_t ack,
                                                  uint8_t flags, const uint8_t* data, uint32_t len,
                                                  Ipv4Module& ipv4, NetStack& stack) {
    const InDevice* cfg = stack.config_for(dev);
    if (cfg == nullptr) {
        return cinux::lib::Error::InvalidArgument;  // no source address for this device
    }
    // One contiguous run = [pseudo-header(12) | TCP header(20) | payload(len)] so
    // internet_checksum over the whole run yields the TCP checksum in one pass
    // (same trick as UdpModule::send).  The pseudo-header never goes on the wire;
    // we emit the TCP portion (past it) via ipv4.send.
    const uint16_t tcp_len = static_cast<uint16_t>(sizeof(TcpHeader) + len);
    HeapBuf        buf(12 + sizeof(TcpHeader) + len);
    build_pseudo_header(buf.p, cfg->local, dst, tcp_len);

    TcpHeader h{};
    h.src_port   = src_port;
    h.dst_port   = dst_port;
    h.seq        = seq;
    h.ack        = ack;
    h.data_off   = kTcpHdrWords;
    h.flags      = flags;
    h.window     = kTcpWindow;
    h.checksum   = 0;
    h.urgent_ptr = 0;
    build_tcp_header(h, buf.p + 12);
    if (len > 0) {
        std::memcpy(buf.p + 12 + sizeof(TcpHeader), data, len);
    }
    uint16_t cs = cinux::lib::internet_checksum(buf.p, 12 + sizeof(TcpHeader) + len);
    if (cs == 0) {
        cs = 0xFFFF;  // TCP mandates a nonzero checksum
    }
    buf.p[12 + 16] = static_cast<uint8_t>(cs >> 8);  // TCP header checksum field
    buf.p[12 + 17] = static_cast<uint8_t>(cs & 0xFF);

    return ipv4.send(dev, dst, kIpProtoTcp, buf.p + 12, sizeof(TcpHeader) + len, stack);
}

// ============================================================
// Active open
// ============================================================

cinux::lib::ErrorOr<void> TcpModule::connect(NetDevice& dev, uint16_t local_port,
                                             Ipv4Addr remote_addr, uint16_t remote_port,
                                             Ipv4Module& ipv4, NetStack& stack) {
    if (find(local_port, remote_addr, remote_port) != nullptr) {
        return cinux::lib::Error::AlreadyExists;  // 4-tuple already connected
    }
    Connection* c = alloc(local_port, remote_addr, remote_port, nullptr);
    if (c == nullptr) {
        return cinux::lib::Error::OutOfMemory;  // table full
    }
    c->iss     = next_isn();
    c->snd_nxt = c->iss + 1;  // our SYN consumes one sequence number
    c->rcv_nxt = 0;           // unknown until the SYN-ACK
    c->state   = TcpState::kSynSent;
    return send_segment(dev, remote_addr, local_port, remote_port, c->iss, 0, kTcpSyn, nullptr, 0,
                        ipv4, stack);
}

// ============================================================
// Data transfer + active close
// ============================================================

cinux::lib::ErrorOr<void> TcpModule::send(NetDevice& dev, uint16_t local_port, Ipv4Addr remote_addr,
                                          uint16_t remote_port, const uint8_t* data, uint32_t len,
                                          Ipv4Module& ipv4, NetStack& stack) {
    Connection* c = find(local_port, remote_addr, remote_port);
    if (c == nullptr || c->state != TcpState::kEstablished) {
        return cinux::lib::Error::InvalidArgument;  // no connection / not established
    }
    const uint32_t seq = c->snd_nxt;
    auto           r   = send_segment(dev, remote_addr, local_port, remote_port, seq, c->rcv_nxt,
                                      kTcpPsh | kTcpAck, data, len, ipv4, stack);
    if (r.ok()) {
        c->snd_nxt += len;  // data consumes len sequence numbers
    }
    return r;
}

cinux::lib::ErrorOr<void> TcpModule::close(NetDevice& dev, uint16_t local_port,
                                           Ipv4Addr remote_addr, uint16_t remote_port,
                                           Ipv4Module& ipv4, NetStack& stack) {
    Connection* c = find(local_port, remote_addr, remote_port);
    if (c == nullptr) {
        return cinux::lib::Error::InvalidArgument;  // no connection
    }
    // FIN is legal from ESTABLISHED (we initiate) or CLOSE_WAIT (peer already closed).
    const bool initiator = c->state == TcpState::kEstablished;
    if (!initiator && c->state != TcpState::kCloseWait) {
        return cinux::lib::Error::InvalidArgument;  // not in a closeable state
    }
    const uint32_t seq = c->snd_nxt;
    auto           r   = send_segment(dev, remote_addr, local_port, remote_port, seq, c->rcv_nxt,
                                      kTcpFin | kTcpAck, nullptr, 0, ipv4, stack);
    if (r.ok()) {
        c->snd_nxt += 1;  // FIN consumes one sequence number
        c->state = initiator ? TcpState::kFinWait1 : TcpState::kLastAck;
    }
    return r;
}

// ============================================================
// Inbound: checksum gate + handshake / data / teardown FSM
// ============================================================

void TcpModule::handle(const Ipv4Header& ip, FrameView payload, NetDevice& dev, Ipv4Module& ipv4,
                       NetStack& stack) {
    if (payload.size() < sizeof(TcpHeader)) {
        return;  // short / not a TCP segment
    }
    // --- checksum gate (mandatory for TCP) ---
    // Reconstruct the run the sender summed -- pseudo-header(12, TCP length = the
    // delivered segment bytes) + the whole segment (with embedded checksum) -- and
    // verify.  The L4 payload IS the whole TCP segment (IPv4 stripped its header),
    // so payload.size() is the TCP length.  Bad checksum -> silent drop.
    const uint16_t tcp_len = static_cast<uint16_t>(payload.size());
    HeapBuf        buf(12 + tcp_len);
    build_pseudo_header(buf.p, ip.src, ip.dst, tcp_len);
    std::memcpy(buf.p + 12, payload.data(), tcp_len);
    if (!cinux::lib::verify_internet_checksum(buf.p, 12 + tcp_len)) {
        return;  // corrupt -> silently drop
    }

    TcpHeader h;
    parse_tcp(payload.data(), h);
    // diagnostics tap (kept from batch 1)
    ++valid_count_;
    last_src_port_ = h.src_port;
    last_dst_port_ = h.dst_port;
    last_seq_      = h.seq;
    last_ack_      = h.ack;
    last_flags_    = h.flags;

    // Malformed-header hardening: reject segments whose declared header length
    // is insane -- data_off < 5 (under 20 bytes) or claiming more header than
    // was delivered.  Without this a data_off of 0 would re-interpret the
    // header bytes as payload, and a data_off past the buffer would mis-anchor
    // the data pointer.  Relevant once M6 makes TCP reachable on real traffic.
    const uint8_t hdrlen = tcp_header_bytes(h);
    if (hdrlen < sizeof(TcpHeader) || hdrlen > payload.size()) {
        return;  // malformed data offset -> drop
    }

    Connection* c = find(h.dst_port, ip.src, h.src_port);

    if (c == nullptr) {
        // No connection for this 4-tuple.
        if (h.flags & kTcpRst) {
            return;  // RST for a non-connection -> ignore
        }
        if (h.flags & kTcpSyn) {
            TcpListener* l = find_listener(h.dst_port);
            if (l != nullptr) {
                // Passive open: SYN to a listened port -> SYN-ACK.
                Connection* nc = alloc(h.dst_port, ip.src, h.src_port, l);
                if (nc == nullptr) {
                    return;  // table full -> drop (no RST; a retry may find room)
                }
                nc->iss     = next_isn();
                nc->rcv_nxt = h.seq + 1;    // peer's SYN consumes one
                nc->snd_nxt = nc->iss + 1;  // our SYN-ACK's SYN consumes one
                nc->state   = TcpState::kSynReceived;
                (void)send_segment(dev, ip.src, h.dst_port, h.src_port, nc->iss, nc->rcv_nxt,
                                   kTcpSyn | kTcpAck, nullptr, 0, ipv4, stack);
            } else {
                // SYN to a closed port -> RST (RFC 793: ack = SEG.SEQ + 1).
                (void)send_segment(dev, ip.src, h.dst_port, h.src_port, 0, h.seq + 1,
                                   kTcpRst | kTcpAck, nullptr, 0, ipv4, stack);
            }
        }
        return;
    }

    // Existing connection.
    if (h.flags & kTcpRst) {
        c->state = TcpState::kClosed;  // RST tears down any connection
        return;
    }

    switch (c->state) {
    case TcpState::kSynSent: {
        // Expecting the SYN-ACK to our SYN.  (Simultaneous open / a pure SYN
        // here is not handled -- minimal viable, no retransmit.)
        if ((h.flags & kTcpSyn) && (h.flags & kTcpAck) && h.ack == c->iss + 1) {
            c->rcv_nxt = h.seq + 1;  // peer's SYN consumes one
            (void)send_segment(dev, ip.src, c->local_port, c->remote_port, c->snd_nxt, c->rcv_nxt,
                               kTcpAck, nullptr, 0, ipv4, stack);
            c->state = TcpState::kEstablished;
        }
        break;
    }
    case TcpState::kSynReceived: {
        // The 3rd packet: ACK completing the passive handshake.
        if ((h.flags & kTcpAck) && !(h.flags & kTcpSyn) && h.ack == c->iss + 1) {
            c->state = TcpState::kEstablished;
            if (c->listener != nullptr) {
                const TcpEndpoint local{ip.dst, c->local_port};  // ip.dst = our addr
                const TcpEndpoint remote{ip.src, c->remote_port};
                c->listener->on_accept(local, remote);
            }
        }
        break;
    }
    case TcpState::kEstablished: {
        // In-order data at the front of the receive window only -- no
        // reassembly / out-of-order (follow-up).  A segment whose SEQ is not
        // RCV.NXT is dropped (a real stack would re-ACK to elicit a retransmit).
        // (hdrlen was validated sane at the top of handle: data_off in
        // [20, payload.size()].)
        if (h.seq != c->rcv_nxt) {
            break;  // out of order -> drop
        }
        const uint32_t data_len = payload.size() - hdrlen;
        bool           need_ack = false;
        if (data_len > 0) {
            if (c->listener != nullptr) {
                const TcpEndpoint local{ip.dst, c->local_port};
                const TcpEndpoint remote{ip.src, c->remote_port};
                c->listener->on_data(local, remote, FrameView(payload.data() + hdrlen, data_len));
            }
            c->rcv_nxt += data_len;
            need_ack = true;
        }
        if (h.flags & kTcpFin) {
            c->rcv_nxt += 1;  // FIN consumes one
            need_ack = true;
            if (c->listener != nullptr) {
                const TcpEndpoint local{ip.dst, c->local_port};
                const TcpEndpoint remote{ip.src, c->remote_port};
                c->listener->on_close(local, remote);
            }
            c->state = TcpState::kCloseWait;
        }
        if (need_ack) {
            (void)send_segment(dev, ip.src, c->local_port, c->remote_port, c->snd_nxt, c->rcv_nxt,
                               kTcpAck, nullptr, 0, ipv4, stack);
        }
        break;
    }
    case TcpState::kFinWait1:
        // Expecting the ACK for our FIN -> FIN_WAIT_2.
        if ((h.flags & kTcpAck) && h.ack == c->snd_nxt) {
            c->state = TcpState::kFinWait2;
        }
        break;
    case TcpState::kFinWait2:
        // Expecting the peer's FIN -> ACK it (seq+1) and we are done.  No
        // TIME_WAIT: no timer here, and the closed peer's final ACK is trusted.
        if (h.flags & kTcpFin) {
            (void)send_segment(dev, ip.src, c->local_port, c->remote_port, c->snd_nxt, h.seq + 1,
                               kTcpAck, nullptr, 0, ipv4, stack);
            c->state = TcpState::kClosed;
        }
        break;
    case TcpState::kCloseWait:
        // Waiting for our app to call close().  A retransmitted peer FIN -> re-ACK.
        if (h.flags & kTcpFin) {
            (void)send_segment(dev, ip.src, c->local_port, c->remote_port, c->snd_nxt, h.seq + 1,
                               kTcpAck, nullptr, 0, ipv4, stack);
        }
        break;
    case TcpState::kLastAck:
        // Our FIN was acked -> connection fully closed.
        if ((h.flags & kTcpAck) && h.ack == c->snd_nxt) {
            c->state = TcpState::kClosed;
        }
        break;
    default:
        break;
    }
}

}  // namespace cinux::net
