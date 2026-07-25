/**
 * @file kernel/net/tcp_socket.hpp
 * @brief TcpSocket -- Socket adapter over TcpModule (F7-M6 batch 3).
 *
 * TcpModule (F7-M5) is a PURE FSM: connect/listen/send/close + a 4-tuple TCB
 * table.  TcpSocket IS-A TcpListener so the modules push inbound lifecycle +
 * data events INTO the socket.  Two roles, one class:
 *
 *   - Listening (server): listen() registers with TcpModule; on_accept() enqueues
 *     a completed peer; accept() dequeues, news a CHILD TcpSocket, and rebinds
 *     that connection's TCB listener to the child (TcpModule::set_listener) so
 *     its on_data/on_close reach the child directly.
 *   - Connected (client after connect(), or an accepted child): holds the peer
 *     endpoint + a byte-stream RX ring (RingBuffer, like pipe).  The socket IS
 *     its TCB's listener, so on_data() copies bytes into the ring and recv()
 *     drains it (blocking on a wait queue, mirroring pipe/UdpSocket).
 *
 * The protocol FSM stays pure; per-fd buffering + blocking live here
 * (CODING-TASTE §13).  Minimal usable (M5 scope): no retransmission/RTO/window/
 * congestion (need a kernel timer); loopback + low-loss SLIRP only.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cinux/ring_buffer.hpp>
#include <cstdint>

#include "kernel/net/net_device.hpp"  // NetDevice
#include "kernel/net/net_stack.hpp"   // NetStack
#include "kernel/net/socket.hpp"      // Socket
#include "kernel/net/tcp.hpp"         // TcpListener, TcpModule, TcpEndpoint
#include "kernel/proc/sync.hpp"       // Spinlock

namespace cinux::proc {
struct Task;  // forward -- wait queues hold blocked recv'ers / accept'ers
}

namespace cinux::net {

class Ipv4Module;

/// @brief TCP socket: byte-stream connection behind a Socket fd, with a
///        pending-accept queue in its listening role.
class TcpSocket : public Socket, public TcpListener {
public:
    using DevRoute = NetDevice& (*)(Ipv4Addr dst);

    /// Listening/server socket (the factory builds these for SOCK_STREAM).
    TcpSocket(TcpModule& tcp, Ipv4Module& ipv4, NetStack& stack, DevRoute route);

    /// Connected socket (an accepted child): same module refs + a known peer.
    /// Public so the parent's accept() can construct one.
    TcpSocket(TcpModule& tcp, Ipv4Module& ipv4, NetStack& stack, DevRoute route,
              uint16_t local_port, Ipv4Addr remote_addr, uint16_t remote_port);

    // --- Socket overrides ---
    cinux::lib::ErrorOr<void>    bind(uint16_t local_port) override;
    cinux::lib::ErrorOr<void>    connect(Ipv4Addr remote, uint16_t remote_port) override;
    cinux::lib::ErrorOr<void>    listen(int backlog) override;
    cinux::lib::ErrorOr<Socket*> accept(Ipv4Addr* out_remote, uint16_t* out_port) override;
    cinux::lib::ErrorOr<int64_t> send(const uint8_t* buf, uint32_t len) override;
    cinux::lib::ErrorOr<int64_t> recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                      uint16_t* out_port) override;
    void                         close() override;

    // --- F-ECO batch 7b: address retrieval (getsockname/getpeername) ---
    bool get_local_addr(SockAddrStorage* out) const override;
    bool get_peer_addr(SockAddrStorage* out) const override;

    // --- F8-M5 poll: listening -> POLLIN on the accept queue; connected ->
    // POLLIN on the rx ring (+ POLLHUP once the peer closes).  Parks on the same
    // wait queue a blocked recv/accept uses.
    uint32_t poll_events(cinux::proc::Task* waiter, bool* registered) override;
    void     poll_detach_waiter(cinux::proc::Task* waiter) override;

    // --- TcpListener: the module pushes connection lifecycle + data in here ---
    void on_accept(const TcpEndpoint& local, const TcpEndpoint& remote) override;
    void on_data(const TcpEndpoint& local, const TcpEndpoint& remote, FrameView data) override;
    void on_close(const TcpEndpoint& local, const TcpEndpoint& remote) override;

private:
    static constexpr uint32_t kRxSize         = 4096;  ///< per-connection byte-stream ring
    static constexpr uint32_t kAcceptMax      = 4;     ///< pending-accept queue depth
    static constexpr uint16_t kEphemeralBase  = 32768;
    static constexpr uint16_t kEphemeralRange = 16;

    TcpModule&  tcp_;
    Ipv4Module& ipv4_;
    NetStack&   stack_;
    DevRoute    route_;

    uint16_t local_port_ = 0;
    Ipv4Addr remote_addr_{};
    uint16_t remote_port_ = 0;
    bool     bound_       = false;
    bool     listening_   = false;
    bool     connected_   = false;
    bool     peer_closed_ = false;

    /// Listening role: peers whose handshake completed, pending accept().
    struct PendingAccept {
        Ipv4Addr addr{};
        uint16_t port = 0;
    };
    PendingAccept accept_queue_[kAcceptMax]{};
    uint32_t      accept_head_ = 0, accept_tail_ = 0, accept_count_ = 0;

    /// Connected role: inbound byte stream (on_data pushes, recv drains).
    cinux::lib::RingBuffer<uint8_t, kRxSize> rx_;

    cinux::proc::Spinlock lock_;
    cinux::proc::Task*    recv_waiters_   = nullptr;  ///< blocked in recv()
    cinux::proc::Task*    accept_waiters_ = nullptr;  ///< blocked in accept()
};

}  // namespace cinux::net
