/**
 * @file kernel/net/tcp_socket.cpp
 * @brief TcpSocket -- byte-stream RX ring + accept queue + blocking (F7-M6 B3).
 *
 * See tcp_socket.hpp.  Blocking mirrors kernel/ipc/pipe.cpp / udp_socket.cpp:
 * recv() parks on recv_waiters_ when the ring is empty (and the peer has not
 * closed); accept() parks on accept_waiters_ when no peer is pending.  on_data
 * / on_accept wake_one() the relevant queue.  Host unit tests compile the
 * blocking out (CINUX_HOST_TEST); a non-blocking recv/accept returns WouldBlock.
 *
 * Minimal usable (no retransmission/RTO): on loopback / low-loss SLIRP a
 * connection exchanges bytes reliably because no segment is lost; a lossy link
 * can drop data (TcpModule has no retransmit -- needs a kernel timer, F5-M4).
 *
 * Namespace: cinux::net
 */

#include "kernel/net/tcp_socket.hpp"
#include "kernel/net/byte_order.hpp"
#include "kernel/net/wait_queue.hpp"  // shared intrusive wait queue (was 3-way duplicated)

#include <cstdint>

#ifndef CINUX_HOST_TEST
#    include "kernel/proc/process.hpp"  // Task + signal_deliverable_pending
#    include "kernel/proc/scheduler.hpp"  // prepare_to_wait/schedule_blocked/unblock
#endif

namespace cinux::net {

#ifndef CINUX_HOST_TEST
using cinux::proc::Scheduler;
using cinux::proc::Task;
#endif




TcpSocket::TcpSocket(TcpModule& tcp, Ipv4Module& ipv4, NetStack& stack, DevRoute route)
    : Socket(kAfInet, kSockStream), tcp_(tcp), ipv4_(ipv4), stack_(stack), route_(route) {}

TcpSocket::TcpSocket(TcpModule& tcp, Ipv4Module& ipv4, NetStack& stack, DevRoute route,
                     uint16_t local_port, Ipv4Addr remote_addr, uint16_t remote_port)
    : Socket(kAfInet, kSockStream),
      tcp_(tcp),
      ipv4_(ipv4),
      stack_(stack),
      route_(route),
      local_port_(local_port),
      remote_addr_(remote_addr),
      remote_port_(remote_port),
      connected_(true) {}

cinux::lib::ErrorOr<void> TcpSocket::bind(uint16_t local_port) {
    auto g      = lock_.irq_guard();
    local_port_ = local_port;
    bound_      = true;
    return {};
}

cinux::lib::ErrorOr<void> TcpSocket::listen(int /*backlog*/) {
    auto g = lock_.irq_guard();
    if (!bound_ || listening_ || connected_) {
        return cinux::lib::Error::InvalidArgument;
    }
    if (!tcp_.listen(local_port_, *this)) {
        return cinux::lib::Error::AlreadyExists;  // port already listened
    }
    listening_ = true;
    return {};
}

cinux::lib::ErrorOr<void> TcpSocket::connect(Ipv4Addr remote, uint16_t remote_port) {
    {
        auto g = lock_.irq_guard();
        if (connected_ || listening_) {
            return cinux::lib::Error::InvalidArgument;
        }
        if (!bound_) {
            // Auto-bind an ephemeral local port (monotonic; collisions are rare
            // at hobby scale -- a real ephemeral allocator is a follow-up).
            static uint16_t eph = kEphemeralBase;
            local_port_         = eph++;
            if (eph >= kEphemeralBase + kEphemeralRange) {
                eph = kEphemeralBase;
            }
            bound_ = true;
        }
        remote_addr_ = remote;
        remote_port_ = remote_port;
        connected_   = true;
    }
    // Active open (sends SYN). The handshake completes on a later poll().
    NetDevice& dev = route_(remote);
    auto       r   = tcp_.connect(dev, local_port_, remote, remote_port, ipv4_, stack_);
    if (!r.ok()) {
        auto g     = lock_.irq_guard();
        connected_ = false;
        return r.error();
    }
    // Rebind this connection's TCB listener to OURSELVES -- M5 leaves active-open
    // TCBs listener=null, so without this the client never receives on_data.
    tcp_.set_listener(local_port_, remote, remote_port, *this);
    return {};
}

cinux::lib::ErrorOr<Socket*> TcpSocket::accept(Ipv4Addr* out_remote, uint16_t* out_port) {
    for (;;) {
#ifndef CINUX_HOST_TEST
        bool need_block = false;
#endif
        {
            auto g = lock_.irq_guard();
            if (accept_count_ > 0) {
                PendingAccept pa = accept_queue_[accept_head_];
                accept_head_     = (accept_head_ + 1) % kAcceptMax;
                --accept_count_;
                // New a connected child for this peer, then rebind the
                // connection's TCB listener from the listening parent to the
                // child so its on_data/on_close reach the child directly.
                auto* child =
                    new TcpSocket(tcp_, ipv4_, stack_, route_, local_port_, pa.addr, pa.port);
                tcp_.set_listener(local_port_, pa.addr, pa.port, *child);
                if (out_remote != nullptr) {
                    *out_remote = pa.addr;
                }
                if (out_port != nullptr) {
                    *out_port = pa.port;
                }
                return static_cast<Socket*>(child);
            }
#ifdef CINUX_HOST_TEST
            return cinux::lib::Error::WouldBlock;
#else
            Task* self = Scheduler::current();
            if (self == nullptr) {
                return cinux::lib::Error::WouldBlock;
            }
            wait_enqueue(accept_waiters_, self);
            Scheduler::prepare_to_wait(self);
            need_block = true;
#endif
        }
#ifndef CINUX_HOST_TEST
        if (need_block) {
            Scheduler::schedule_blocked();
        }
        // EINTR: a signal landed while parked waiting for a peer.  Return the
        // sentinel (-1 cast to Socket*) so do_accept maps it to -EINTR.  Unlink
        // under lock_ first so a producer does not wake a stale link.
        if (Scheduler::current() != nullptr &&
            signal_deliverable_pending(Scheduler::current())) {
            auto g = lock_.irq_guard();
            wait_remove(accept_waiters_, Scheduler::current());
            return reinterpret_cast<Socket*>(static_cast<uintptr_t>(-1));  // EINTR sentinel
        }
#endif
    }
}

cinux::lib::ErrorOr<int64_t> TcpSocket::send(const uint8_t* buf, uint32_t len) {
    if (shut_write()) {  // SHUT_WR recorded -> EPIPE-shaped
        return cinux::lib::Error::BrokenPipe;
    }
    if (buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.irq_guard();
    if (!connected_ || peer_closed_) {
        return cinux::lib::Error::InvalidArgument;  // ENOTCONN-shaped
    }
    uint32_t   n   = len > kRxSize ? kRxSize : len;  // cap to one segment/ring's worth
    NetDevice& dev = route_(remote_addr_);
    auto       r   = tcp_.send(dev, local_port_, remote_addr_, remote_port_, buf, n, ipv4_, stack_);
    if (!r.ok()) {
        return r.error();
    }
    return static_cast<int64_t>(n);
}

cinux::lib::ErrorOr<int64_t> TcpSocket::recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                             uint16_t* out_port) {
    if (buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    if (shut_read()) {  // SHUT_RD recorded -> EOF
        return static_cast<int64_t>(0);
    }
    for (;;) {
#ifndef CINUX_HOST_TEST
        bool need_block = false;
#endif
        {
            auto g = lock_.irq_guard();
            if (rx_.size() > 0) {
                uint32_t want = len < rx_.size() ? len : static_cast<uint32_t>(rx_.size());
                uint32_t got  = static_cast<uint32_t>(rx_.pop_batch(buf, want));
                if (out_src != nullptr) {
                    *out_src = remote_addr_;
                }
                if (out_port != nullptr) {
                    *out_port = remote_port_;
                }
                return static_cast<int64_t>(got);
            }
            if (peer_closed_) {
                return static_cast<int64_t>(0);  // EOF: peer closed + ring drained
            }
#ifdef CINUX_HOST_TEST
            return cinux::lib::Error::WouldBlock;
#else
            Task* self = Scheduler::current();
            if (self == nullptr) {
                return cinux::lib::Error::WouldBlock;
            }
            wait_enqueue(recv_waiters_, self);
            Scheduler::prepare_to_wait(self);
            need_block = true;
#endif
        }
#ifndef CINUX_HOST_TEST
        if (need_block) {
            Scheduler::schedule_blocked();
        }
        // EINTR: a signal landed while parked.  Return sentinel -1 (a value a
        // real byte-count can never take) so sys_recvfrom maps it to -EINTR.
        // Unlink under lock_ first so a producer does not wake a stale link.
        if (Scheduler::current() != nullptr &&
            signal_deliverable_pending(Scheduler::current())) {
            auto g = lock_.irq_guard();
            wait_remove(recv_waiters_, Scheduler::current());
            return static_cast<int64_t>(-1);  // sentinel: sys_recvfrom -> -EINTR
        }
#endif
    }
}

void TcpSocket::on_accept(const TcpEndpoint& /*local*/, const TcpEndpoint& remote) {
    auto g = lock_.irq_guard();
    if (accept_count_ >= kAcceptMax) {
        return;  // accept queue full -> drop (no backpressure yet)
    }
    accept_queue_[accept_tail_].addr = remote.addr;
    accept_queue_[accept_tail_].port = remote.port;
    accept_tail_                     = (accept_tail_ + 1) % kAcceptMax;
    ++accept_count_;
#ifndef CINUX_HOST_TEST
    wake_one(accept_waiters_);
#endif
}

void TcpSocket::on_data(const TcpEndpoint& /*local*/, const TcpEndpoint& /*remote*/,
                        FrameView data) {
    auto g = lock_.irq_guard();
    if (rx_.full()) {
        return;  // ring full -> drop (no flow control yet)
    }
    uint32_t space = kRxSize - static_cast<uint32_t>(rx_.size());
    uint32_t n     = data.size() < space ? static_cast<uint32_t>(data.size()) : space;
    rx_.push_batch(data.data(), n);
#ifndef CINUX_HOST_TEST
    wake_one(recv_waiters_);
#endif
}

void TcpSocket::on_close(const TcpEndpoint& /*local*/, const TcpEndpoint& /*remote*/) {
    auto g       = lock_.irq_guard();
    peer_closed_ = true;
#ifndef CINUX_HOST_TEST
    wake_all(recv_waiters_);  // blocked recv'ers retry -> drained ring -> EOF
#endif
}

void TcpSocket::close() {
    auto g = lock_.irq_guard();
    if (listening_) {
        tcp_.stop_listen(local_port_);
        listening_ = false;
    }
    if (connected_ && !peer_closed_) {
        NetDevice& dev = route_(remote_addr_);
        static_cast<void>(tcp_.close(dev, local_port_, remote_addr_, remote_port_, ipv4_, stack_));
    }
    peer_closed_ = true;
#ifndef CINUX_HOST_TEST
    wake_all(recv_waiters_);
    wake_all(accept_waiters_);
#endif
}

uint32_t TcpSocket::poll_events([[maybe_unused]] cinux::proc::Task* waiter, bool* registered) {
    auto g = lock_.irq_guard();
    if (registered != nullptr) {
        *registered = (waiter != nullptr);
    }
    uint32_t mask = 0;
    if (listening_) {
        // Server: readable when a completed connection is pending accept().
        if (accept_count_ > 0) {
            mask |= cinux::fs::kPollIn;
        }
#ifndef CINUX_HOST_TEST
        if (waiter != nullptr) {
            wait_enqueue(accept_waiters_, waiter);
        }
    } else if (connected_) {
        // Client: readable while bytes are buffered; POLLHUP once the peer closes.
        if (rx_.size() > 0) {
            mask |= cinux::fs::kPollIn;
        }
        if (peer_closed_) {
            mask |= cinux::fs::kPollHup;
        }
        mask |= cinux::fs::kPollOut;  // connected -> writable
        if (waiter != nullptr) {
            wait_enqueue(recv_waiters_, waiter);
        }
    }
#else
#endif
    return mask;
}

void TcpSocket::poll_detach_waiter([[maybe_unused]] cinux::proc::Task* waiter) {
#ifndef CINUX_HOST_TEST
    auto g = lock_.irq_guard();
    // A poller parks on at most one of the two queues (per listening/connected
    // state); removing from both is a harmless no-op on the empty one.
    wait_remove(recv_waiters_, waiter);
    wait_remove(accept_waiters_, waiter);
#else
#endif
}

bool TcpSocket::get_local_addr(SockAddrStorage* out) const {
    if (!(bound_ || connected_)) {
        return false;  // not bound/connected -> no local name yet
    }
    SockAddrIn sin{};
    sin.family  = kAfInet;
    sin.port    = byte_swap16(local_port_);                     // network order
    sin.addr[0] = sin.addr[1] = sin.addr[2] = sin.addr[3] = 0;  // INADDR_ANY
    __builtin_memcpy(out->bytes, &sin, sizeof(sin));
    return true;
}

bool TcpSocket::get_peer_addr(SockAddrStorage* out) const {
    if (!connected_) {
        return false;  // no peer until connect()/accept() completes
    }
    SockAddrIn sin{};
    sin.family  = kAfInet;
    sin.port    = byte_swap16(remote_port_);  // network order
    sin.addr[0] = remote_addr_.oct[0];
    sin.addr[1] = remote_addr_.oct[1];
    sin.addr[2] = remote_addr_.oct[2];
    sin.addr[3] = remote_addr_.oct[3];
    __builtin_memcpy(out->bytes, &sin, sizeof(sin));
    return true;
}

}  // namespace cinux::net
