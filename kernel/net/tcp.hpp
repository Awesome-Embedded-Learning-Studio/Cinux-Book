/**
 * @file kernel/net/tcp.hpp
 * @brief TCP wire layout + TcpModule (L4Handler, IP proto 6).
 *
 * A connection-oriented L4 protocol layered on Ipv4Module exactly like
 * UdpModule: TcpModule is an L4Handler registered via
 * Ipv4Module::add_l4(kIpProtoTcp, ...).  Batch 1 delivers the wire format
 * (header parse/build + flag constants) and the inbound checksum gate: handle()
 * verifies the TCP pseudo-header checksum (proto 6) and records the segment for
 * diagnostics.  Unlike UDP a TCP checksum is MANDATORY -- 0 is a protocol
 * violation, not "no checksum" -- so the gate always verifies.  The connection
 * state machine (handshake / sequence-ACK / teardown) is layered on in
 * batches 2-3; the socket API rides on top in F7-M6.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/ipv4.hpp"  // Ipv4Header, Ipv4Module, L4Handler, kIpProto*
#include "kernel/net/net_types.hpp"

namespace cinux::net {

/// TCP header length in 32-bit words (fixed 20-byte header, no options today).
constexpr uint8_t kTcpHdrWords = 5;

/// TCP flag bits (wire byte 13).  SYN/ACK/FIN/RST drive the FSM; PSH/URG are
/// accepted but not semantically required at this layer.
constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpPsh = 0x08;
constexpr uint8_t kTcpAck = 0x10;
constexpr uint8_t kTcpUrg = 0x20;

/// @brief TCP header, HOST-order parsed view.  20 bytes (data offset = 5).
struct TcpHeader {
    uint16_t src_port;    ///< host order
    uint16_t dst_port;    ///< host order
    uint32_t seq;         ///< host order (sequence number)
    uint32_t ack;         ///< host order (acknowledgment number)
    uint8_t  data_off;    ///< header length in 32-bit words (raw 4-bit value)
    uint8_t  flags;       ///< FIN..CWR (low 6 bits used: kTcp*)
    uint16_t window;      ///< host order (receive window, bytes)
    uint16_t checksum;    ///< host order (pseudo-header based; MANDATORY)
    uint16_t urgent_ptr;  ///< host order
};
static_assert(sizeof(TcpHeader) == 20, "TCP fixed header is 20 bytes");

/// @brief Parse 20 wire bytes into a TcpHeader (host order).  Options bytes
///        (data_off > 5) are not parsed; callers skip them via tcp_header_bytes.
inline void parse_tcp(const uint8_t* p, TcpHeader& out) {
    out.src_port   = (static_cast<uint16_t>(p[0]) << 8) | p[1];
    out.dst_port   = (static_cast<uint16_t>(p[2]) << 8) | p[3];
    out.seq        = (static_cast<uint32_t>(p[4]) << 24) | (static_cast<uint32_t>(p[5]) << 16) |
                     (static_cast<uint32_t>(p[6]) << 8) | p[7];
    out.ack        = (static_cast<uint32_t>(p[8]) << 24) | (static_cast<uint32_t>(p[9]) << 16) |
                     (static_cast<uint32_t>(p[10]) << 8) | p[11];
    out.data_off   = static_cast<uint8_t>(p[12] >> 4);
    out.flags      = p[13];
    out.window     = (static_cast<uint16_t>(p[14]) << 8) | p[15];
    out.checksum   = (static_cast<uint16_t>(p[16]) << 8) | p[17];
    out.urgent_ptr = (static_cast<uint16_t>(p[18]) << 8) | p[19];
}

/// @brief Serialise a TcpHeader into 20 wire bytes (big-endian multi-byte; the
///        data offset sits in the hi nibble of byte 12, flags in byte 13).
inline void build_tcp_header(const TcpHeader& in, uint8_t* p) {
    p[0]  = static_cast<uint8_t>(in.src_port >> 8);
    p[1]  = static_cast<uint8_t>(in.src_port & 0xFF);
    p[2]  = static_cast<uint8_t>(in.dst_port >> 8);
    p[3]  = static_cast<uint8_t>(in.dst_port & 0xFF);
    p[4]  = static_cast<uint8_t>(in.seq >> 24);
    p[5]  = static_cast<uint8_t>(in.seq >> 16);
    p[6]  = static_cast<uint8_t>(in.seq >> 8);
    p[7]  = static_cast<uint8_t>(in.seq & 0xFF);
    p[8]  = static_cast<uint8_t>(in.ack >> 24);
    p[9]  = static_cast<uint8_t>(in.ack >> 16);
    p[10] = static_cast<uint8_t>(in.ack >> 8);
    p[11] = static_cast<uint8_t>(in.ack & 0xFF);
    p[12] = static_cast<uint8_t>(in.data_off << 4);  // reserved lo nibble zero
    p[13] = in.flags;
    p[14] = static_cast<uint8_t>(in.window >> 8);
    p[15] = static_cast<uint8_t>(in.window & 0xFF);
    p[16] = static_cast<uint8_t>(in.checksum >> 8);
    p[17] = static_cast<uint8_t>(in.checksum & 0xFF);
    p[18] = static_cast<uint8_t>(in.urgent_ptr >> 8);
    p[19] = static_cast<uint8_t>(in.urgent_ptr & 0xFF);
}

/// @brief Header length in bytes (data offset * 4).
inline uint8_t tcp_header_bytes(const TcpHeader& h) {
    return static_cast<uint8_t>(h.data_off * 4);
}

/// A connection endpoint: an address + port.  The FSM keys connections on the
/// local/remote endpoint pair (the TCP 4-tuple, minus the local addr which the
/// device's InDevice already pins).
struct TcpEndpoint {
    Ipv4Addr addr;
    uint16_t port;
};

/// Connection states the FSM moves through.  Batch 2 covers the handshake half
/// (Closed/SynSent/SynReceived/Established); batch 3 adds the teardown half
/// (FinWait1/FinWait2/CloseWait/LastAck).  TIME_WAIT needs a timer (absent here)
/// -- a closed peer's final ACK is trusted, no retransmit (follow-up).
enum class TcpState : uint8_t {
    kClosed      = 0,
    kSynSent     = 1,
    kSynReceived = 2,
    kEstablished = 3,
    kFinWait1    = 4,
    kFinWait2    = 5,
    kCloseWait   = 6,
    kLastAck     = 7,
};

/// @brief Inbound TCP observer -- the consumer of a connection's lifecycle.
///
/// Registered with TcpModule::listen.  Like all net-stack callbacks, @p data is
/// BORROWED: valid only for the duration of the call -- copy to retain (the
/// device recycles the buffer after dispatch).
class TcpListener {
public:
    virtual ~TcpListener() = default;

    /// @brief A passive open completed: a SYN to a listened port finished the
    ///        handshake (the 3rd ACK landed).  @p local is the listened
    ///        endpoint, @p remote the peer.
    virtual void on_accept(const TcpEndpoint& local, const TcpEndpoint& remote) = 0;
    /// @brief Inbound data on an established connection (batch 3).  Default:
    ///        ignore.  @p data is borrowed.
    virtual void on_data(const TcpEndpoint& /*local*/, const TcpEndpoint& /*remote*/,
                         FrameView /*data*/) {}
    /// @brief The connection ended -- peer FIN or RST (batch 3).  Default: ignore.
    virtual void on_close(const TcpEndpoint& /*local*/, const TcpEndpoint& /*remote*/) {}
};

/// @brief TCP protocol layer.  Batch 1: wire + checksum gate.  Batch 2: the
///        connection table + 3-way handshake.  Batch 3: data + 4-way teardown.
class TcpModule : public L4Handler {
public:
    static constexpr uint32_t kMaxTcpCons = 8;  ///< connection-table size

    /// @brief Open a passive listener on @p local_port (mirrors UDP bind).
    /// @return false if @p local_port is already listened or the table is full.
    bool listen(uint16_t local_port, TcpListener& listener);
    /// @brief Release a passive listener.  No-op if @p local_port is not listened.
    void stop_listen(uint16_t local_port);

    /// @brief Active open: send SYN to @p remote, enter SYN_SENT.  The handshake
    ///        completes on the next inbound SYN-ACK (drained by poll).  Fails if a
    ///        connection on this 4-tuple already exists or the table is full.
    cinux::lib::ErrorOr<void> connect(NetDevice& dev, uint16_t local_port, Ipv4Addr remote_addr,
                                      uint16_t remote_port, Ipv4Module& ipv4, NetStack& stack);

    /// @brief Send @p data on an ESTABLISHED connection (PSH|ACK).  Advances
    ///        SND.NXT by @p len; the peer's ACK (drained by poll) acknowledges it.
    ///        Fails if no such connection or it is not ESTABLISHED.
    cinux::lib::ErrorOr<void> send(NetDevice& dev, uint16_t local_port, Ipv4Addr remote_addr,
                                   uint16_t remote_port, const uint8_t* data, uint32_t len,
                                   Ipv4Module& ipv4, NetStack& stack);

    /// @brief Active close: send FIN.  From ESTABLISHED -> FIN_WAIT_1 (we
    ///        initiate); from CLOSE_WAIT -> LAST_ACK (we finish after the peer's
    ///        FIN).  The teardown completes as the remaining FIN/ACK exchange is
    ///        drained by poll.  Fails if no such connection or not in a closeable
    ///        state.
    cinux::lib::ErrorOr<void> close(NetDevice& dev, uint16_t local_port, Ipv4Addr remote_addr,
                                    uint16_t remote_port, Ipv4Module& ipv4, NetStack& stack);

    /// @brief L4Handler: verify the pseudo-header checksum, then advance the FSM
    ///        for the matching connection (passive-open a SYN to a listened port,
    ///        RST an unroutable segment, complete the handshake on SYN-ACK/ACK).
    ///        A bad checksum is a silent drop (like UDP/ICMP).
    void handle(const Ipv4Header& ip, FrameView payload, NetDevice& dev, Ipv4Module& ipv4,
                NetStack& stack) override;

    // --- test observation ---
    uint32_t connection_count() const;  ///< non-Closed connections in the table
    TcpState state_of(uint16_t local_port, Ipv4Addr remote_addr, uint16_t remote_port) const;
    // checksum-gate diagnostics (kept from batch 1 as a lightweight tap)
    uint32_t valid_count() const { return valid_count_; }
    uint16_t last_src_port() const { return last_src_port_; }
    uint16_t last_dst_port() const { return last_dst_port_; }
    uint32_t last_seq() const { return last_seq_; }
    uint32_t last_ack() const { return last_ack_; }
    uint8_t  last_flags() const { return last_flags_; }
    void     reset_diag() {
        valid_count_   = 0;
        last_src_port_ = 0;
        last_dst_port_ = 0;
        last_seq_      = 0;
        last_ack_      = 0;
        last_flags_    = 0;
    }

private:
    /// One connection record (TCB).  state==kClosed marks a free table slot.
    struct Connection {
        TcpState     state      = TcpState::kClosed;
        uint16_t     local_port = 0;
        Ipv4Addr     remote_addr{};
        uint16_t     remote_port = 0;
        uint32_t     iss         = 0;        ///< our initial send sequence
        uint32_t     snd_nxt     = 0;        ///< next seq we send
        uint32_t     rcv_nxt     = 0;        ///< next seq we expect to receive (what we ACK)
        TcpListener* listener    = nullptr;  ///< set on passive open
    };
    struct ListenSlot {
        uint16_t     port = 0;
        TcpListener* l    = nullptr;
    };

    Connection*  find(uint16_t local_port, Ipv4Addr remote_addr, uint16_t remote_port);
    Connection*  alloc(uint16_t local_port, Ipv4Addr remote_addr, uint16_t remote_port,
                       TcpListener* listener);
    TcpListener* find_listener(uint16_t port);
    uint32_t     next_isn();  ///< deterministic ISN (randomization is a follow-up)

    /// @brief Build + checksum + emit one TCP segment via ipv4.send (proto 6).
    ///        The pseudo-header is sourced from the device's InDevice (local) +
    ///        @p dst.  Data is optional (handshake/control segments pass len=0).
    cinux::lib::ErrorOr<void> send_segment(NetDevice& dev, Ipv4Addr dst, uint16_t src_port,
                                           uint16_t dst_port, uint32_t seq, uint32_t ack,
                                           uint8_t flags, const uint8_t* data, uint32_t len,
                                           Ipv4Module& ipv4, NetStack& stack);

    Connection cons_[kMaxTcpCons]{};
    ListenSlot listens_[kMaxTcpCons]{};
    uint32_t   isn_counter_   = 0x00004000;  ///< ISN source; bumped by 64K per connection
    uint32_t   valid_count_   = 0;           ///< segments past the checksum gate
    uint16_t   last_src_port_ = 0;           ///< diagnostics tap (last valid segment)
    uint16_t   last_dst_port_ = 0;
    uint32_t   last_seq_      = 0;
    uint32_t   last_ack_      = 0;
    uint8_t    last_flags_    = 0;
};

}  // namespace cinux::net
