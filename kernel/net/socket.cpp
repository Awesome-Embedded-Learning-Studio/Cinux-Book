/**
 * @file kernel/net/socket.cpp
 * @brief Socket base default virtuals + SocketOps InodeOps shim (F7-M6 batch 1b).
 *
 * Batch 1b ships only the plumbing: the Socket base stubs (every protocol op
 * returns Error::NotImplemented) + the SocketOps shim (read->recv, write->send).
 * UdpSocket (B2) / TcpSocket (B3) override the virtuals to do real work; the
 * stubs make the fd + syscall machinery exercisable first, in isolation.
 *
 * This file stays decoupled (CODING-TASTE §14 + check_net_decoupling.sh): it
 * pulls in no driver / dma / arch-irq header -- only the net types + the fs
 * InodeOps seam.  The blocking wait-queue path (B2/B3) is scheduler-coupled and
 * host-guarded there, mirroring kernel/ipc/pipe.cpp.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/socket.hpp"

namespace cinux::net {

// ============================================================
// Socket base -- default (stub) virtuals.  Overridden by B2/B3.
// ============================================================

cinux::lib::ErrorOr<void> Socket::bind(uint16_t /*local_port*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<void> Socket::connect(Ipv4Addr /*remote*/, uint16_t /*remote_port*/) {
    return cinux::lib::Error::NotImplemented;
}
// AF_UNIX path-shaped variants -- default stubs.  Overridden by UnixSocket.
cinux::lib::ErrorOr<void> Socket::bind_path(const char* /*path*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<void> Socket::connect_path(const char* /*path*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<void> Socket::listen(int /*backlog*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<Socket*> Socket::accept(Ipv4Addr* /*out_remote*/, uint16_t* /*out_port*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<int64_t> Socket::send(const uint8_t* /*buf*/, uint32_t /*len*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<int64_t> Socket::sendto(Ipv4Addr /*dst*/, uint16_t /*dst_port*/,
                                            const uint8_t* /*buf*/, uint32_t /*len*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<int64_t> Socket::recv(uint8_t* /*buf*/, uint32_t /*len*/, Ipv4Addr* /*out_src*/,
                                          uint16_t* /*out_port*/) {
    return cinux::lib::Error::NotImplemented;
}
void Socket::close() {}

// F-ECO batch 7b: address retrieval + shutdown state.  Defaults: no name.  The
// do_shutdown bits live here (base); subclass send()/recv() consult shut_read()/
// shut_write() at entry.
bool Socket::get_local_addr(SockAddrStorage* /*out*/) const {
    return false;
}
bool Socket::get_peer_addr(SockAddrStorage* /*out*/) const {
    return false;
}
void Socket::do_shutdown(int how) {
    if (how == kShutRd || how == kShutRdwr) {
        shut_ |= kShutRdBit;
    }
    if (how == kShutWr || how == kShutRdwr) {
        shut_ |= kShutWrBit;
    }
}

// ============================================================
// SocketOps -- the InodeOps shim (one shared, stateless instance).
// ============================================================

SocketOps& socket_ops() {
    static SocketOps instance;
    return instance;
}

cinux::lib::ErrorOr<int64_t> SocketOps::read(const cinux::fs::Inode* inode, uint64_t /*offset*/,
                                             void* buf, uint64_t count) {
    auto* s = socket_of(inode);
    if (s == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto r = s->recv(static_cast<uint8_t*>(buf), static_cast<uint32_t>(count), nullptr, nullptr);
    if (!r.ok()) {
        return r.error();
    }
    return *r;
}

cinux::lib::ErrorOr<int64_t> SocketOps::write(cinux::fs::Inode* inode, uint64_t /*offset*/,
                                              const void* buf, uint64_t count) {
    auto* s = socket_of(inode);
    if (s == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto r = s->send(static_cast<const uint8_t*>(buf), static_cast<uint32_t>(count));
    if (!r.ok()) {
        return r.error();
    }
    return *r;
}

// ============================================================
// F8-M5: poll readiness.  Socket base default + SocketOps delegation.
// ============================================================

// Base default: a bare/unconnected socket reports nothing ready and never parks
// a poller.  Udp/Tcp/Unix override to inspect their rx ring / accept queue and
// register on the wait queue a blocked recv/accept already uses.
uint32_t Socket::poll_events(cinux::proc::Task*, bool* registered) {
    if (registered != nullptr) {
        *registered = false;
    }
    return 0;
}

void Socket::poll_detach_waiter(cinux::proc::Task*) {
    // Base never registers; nothing to remove.
}

uint32_t SocketOps::poll_events(const cinux::fs::Inode* inode, cinux::proc::Task* waiter,
                                bool* registered) {
    auto* s = socket_of(inode);
    if (s == nullptr) {
        if (registered != nullptr) {
            *registered = false;
        }
        return cinux::fs::kPollErr;  // no Socket bound to this inode -> error
    }
    return s->poll_events(waiter, registered);
}

void SocketOps::poll_detach_waiter(const cinux::fs::Inode* inode, cinux::proc::Task* waiter) {
    auto* s = socket_of(inode);
    if (s != nullptr) {
        s->poll_detach_waiter(waiter);
    }
}

void SocketOps::release(cinux::fs::Inode* inode) {
    // Closing a socket fd: release the protocol resources (unbind the port,
    // stop listening, send FIN).  Each socket fd owns exactly one Socket, so a
    // plain close -> Socket::close() is correct without inode refcounting.
    auto* s = socket_of(inode);
    if (s != nullptr) {
        s->close();
    }
}

}  // namespace cinux::net
