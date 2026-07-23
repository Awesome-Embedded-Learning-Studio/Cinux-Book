/**
 * @file kernel/net/udp_socket.cpp
 * @brief UdpSocket -- per-fd datagram ring + blocking recv (F7-M6 batch 2).
 *
 * See udp_socket.hpp.  Blocking mirrors kernel/ipc/pipe.cpp: recv() on an empty
 * ring parks on recv_waiters_ via prepare_to_wait/schedule_blocked; on_udp()
 * wake_one()s it after enqueuing.  Host unit tests (CINUX_HOST_TEST) compile the
 * blocking path out and recv() returns WouldBlock on an empty ring instead.
 *
 * Error mapping note: cinux::lib::Error has no socket-specific variants
 * (ENOTCONN/EADDRNOTAVAIL), so this layer uses the closest generic Error; the
 * syscall handler's to_errno() turns those into -EINVAL/-EBUSY.  B4 can refine
 * if a musl program's errno sensitivity demands it.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/udp_socket.hpp"

#include <cstdint>

#ifndef CINUX_HOST_TEST
#    include "kernel/proc/process.hpp"    // Task::wait_next
#    include "kernel/proc/scheduler.hpp"  // prepare_to_wait/schedule_blocked/unblock
#endif

namespace cinux::net {

namespace {
/// Ephemeral source-port range for auto-binding an unbound UDP socket on sendto
/// (Linux behaviour).  A small window matches the protocol layer's table cap.
constexpr uint16_t kEphemeralBase  = 32768;
constexpr uint16_t kEphemeralRange = 16;

/// Host -> network byte order for 16-bit (sockaddr_in::port is big-endian, the
/// same layout musl lays out in user space; getsockname/getpeername hand back
/// the wire-order value).  Local helper -- sys_socket.cpp has its own; do not
/// reach into it.
constexpr uint16_t byte_swap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}
}  // namespace

#ifndef CINUX_HOST_TEST
namespace {
using cinux::proc::Scheduler;
using cinux::proc::Task;

void wait_enqueue(Task*& head, Task* t) {
    t->wait_next = nullptr;
    if (head == nullptr) {
        head = t;
        return;
    }
    Task* x = head;
    while (x->wait_next != nullptr) {
        x = x->wait_next;
    }
    x->wait_next = t;
}

Task* wait_dequeue(Task*& head) {
    Task* t = head;
    if (t != nullptr) {
        head         = t->wait_next;
        t->wait_next = nullptr;
    }
    return t;
}

void wake_one(Task*& head) {
    if (Task* t = wait_dequeue(head)) {
        Scheduler::unblock(t);
    }
}

void wake_all(Task*& head) {
    while (Task* t = wait_dequeue(head)) {
        Scheduler::unblock(t);
    }
}
}  // namespace
#endif  // CINUX_HOST_TEST

UdpSocket::UdpSocket(UdpModule& udp, Ipv4Module& ipv4, NetStack& stack, DevRoute route)
    : Socket(kAfInet, kSockDgram), udp_(udp), ipv4_(ipv4), stack_(stack), route_(route) {}

cinux::lib::ErrorOr<void> UdpSocket::bind(uint16_t local_port) {
    auto g = lock_.irq_guard();
    if (bound_) {
        return cinux::lib::Error::AlreadyExists;  // -EEXIST (B4: map to EADDRINUSE)
    }
    if (!udp_.bind(local_port, *this)) {
        return cinux::lib::Error::AlreadyExists;  // port taken -> EADDRINUSE-shaped
    }
    local_port_ = local_port;
    bound_      = true;
    return {};
}

cinux::lib::ErrorOr<void> UdpSocket::connect(Ipv4Addr remote, uint16_t remote_port) {
    // UDP "connect" just fixes a default peer -- no handshake, no FSM.
    peer_addr_ = remote;
    peer_port_ = remote_port;
    connected_ = true;
    return {};
}

cinux::lib::ErrorOr<int64_t> UdpSocket::send(const uint8_t* buf, uint32_t len) {
    if (shut_write()) {
        return cinux::lib::Error::BrokenPipe;  // SHUT_WR recorded -> EPIPE
    }
    if (!connected_) {
        return cinux::lib::Error::InvalidArgument;  // ENOTCONN-shaped (no Socket peer)
    }
    return sendto(peer_addr_, peer_port_, buf, len);
}

cinux::lib::ErrorOr<int64_t> UdpSocket::sendto(Ipv4Addr dst, uint16_t dst_port, const uint8_t* buf,
                                               uint32_t len) {
    if (shut_write()) {
        return cinux::lib::Error::BrokenPipe;  // SHUT_WR recorded -> EPIPE
    }
    if (buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    uint16_t n = len > kMaxDgram ? kMaxDgram : static_cast<uint16_t>(len);

    // Auto-bind an ephemeral source port if the caller never bound (Linux sends
    // from an unbound UDP socket by assigning an ephemeral port on first send).
    if (!bound_) {
        bool ok = false;
        {
            auto g = lock_.irq_guard();
            for (uint16_t p = kEphemeralBase; p < kEphemeralBase + kEphemeralRange; ++p) {
                if (udp_.bind(p, *this)) {
                    local_port_ = p;
                    bound_      = true;
                    ok          = true;
                    break;
                }
            }
        }
        if (!ok) {
            return cinux::lib::Error::Busy;  // no free ephemeral port
        }
    }

    NetDevice& dev = route_(dst);
    auto       r   = udp_.send(dev, dst, local_port_, dst_port, buf, n, ipv4_, stack_);
    if (!r.ok()) {
        return r.error();
    }
    return static_cast<int64_t>(n);
}

cinux::lib::ErrorOr<int64_t> UdpSocket::recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                             uint16_t* out_port) {
    if (shut_read()) {
        return static_cast<int64_t>(0);  // SHUT_RD recorded -> EOF (0 bytes)
    }
    if (buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    for (;;) {
#ifndef CINUX_HOST_TEST
        bool need_block = false;
#endif
        {
            auto g = lock_.irq_guard();
            if (rx_count_ > 0) {
                Datagram& dg = rx_[rx_head_];
                uint32_t  n  = len < dg.len ? len : dg.len;
                for (uint32_t i = 0; i < n; ++i) {
                    buf[i] = dg.data[i];
                }
                if (out_src != nullptr) {
                    *out_src = dg.src;
                }
                if (out_port != nullptr) {
                    *out_port = dg.src_port;
                }
                rx_head_ = (rx_head_ + 1) % kRxSlots;
                --rx_count_;
                return static_cast<int64_t>(n);  // one datagram per recv (truncated to len)
            }
#ifdef CINUX_HOST_TEST
            return cinux::lib::Error::WouldBlock;  // host: no scheduler, never block
#else
            Task* self = Scheduler::current();
            if (self == nullptr) {
                return cinux::lib::Error::WouldBlock;  // no scheduler context (early boot)
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
        // Woken by on_udp() enqueuing a datagram; loop and dequeue.
#endif
    }
}

void UdpSocket::on_udp(const Ipv4Header& ip, uint16_t src_port, FrameView payload) {
    auto g = lock_.irq_guard();
    if (rx_count_ >= kRxSlots) {
        return;  // ring full -> drop (no flow control / backpressure yet)
    }
    Datagram& dg = rx_[rx_tail_];
    dg.src       = ip.src;
    dg.src_port  = src_port;
    uint32_t n   = payload.size() < kMaxDgram ? static_cast<uint32_t>(payload.size()) : kMaxDgram;
    dg.len       = static_cast<uint16_t>(n);
    for (uint32_t i = 0; i < n; ++i) {
        dg.data[i] = payload[i];
    }
    rx_tail_ = (rx_tail_ + 1) % kRxSlots;
    ++rx_count_;
#ifndef CINUX_HOST_TEST
    wake_one(recv_waiters_);  // a blocked recv can now dequeue
#endif
}

void UdpSocket::close() {
    auto g = lock_.irq_guard();
    if (bound_) {
        udp_.unbind(local_port_);
        bound_ = false;
    }
#ifndef CINUX_HOST_TEST
    wake_all(recv_waiters_);  // blocked recv'ers retry -> empty ring -> WouldBlock
#endif
}

bool UdpSocket::get_local_addr(SockAddrStorage* out) const {
    // getsockname: report the bound local endpoint.  We bind INADDR_ANY (all
    // interfaces), so the address field is 0.0.0.0 -- there is no per-interface
    // selection yet.  Unbound -> false (the syscall answers -EINVAL/unnamed).
    if (!bound_ || out == nullptr) {
        return false;
    }
    SockAddrIn sa{};
    sa.family = kAfInet;
    sa.port   = byte_swap16(local_port_);  // wire order
    // sa.addr stays 0,0,0,0 (INADDR_ANY); sa.zero already cleared by {}.
    __builtin_memcpy(out->bytes, &sa, sizeof(sa));
    return true;
}

bool UdpSocket::get_peer_addr(SockAddrStorage* out) const {
    // getpeername: report the fixed peer from a prior connect().  UDP connect()
    // only records the default destination (no handshake), so "connected" IS the
    // predicate for having a peer.  Unconnected -> false (-ENOTCONN).
    if (!connected_ || out == nullptr) {
        return false;
    }
    SockAddrIn sa{};
    sa.family  = kAfInet;
    sa.port    = byte_swap16(peer_port_);  // wire order
    sa.addr[0] = peer_addr_.oct[0];
    sa.addr[1] = peer_addr_.oct[1];
    sa.addr[2] = peer_addr_.oct[2];
    sa.addr[3] = peer_addr_.oct[3];
    __builtin_memcpy(out->bytes, &sa, sizeof(sa));
    return true;
}

}  // namespace cinux::net
