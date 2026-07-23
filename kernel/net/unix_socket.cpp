/**
 * @file kernel/net/unix_socket.cpp
 * @brief AF_UNIX socket + in-memory name registry (F8-M3).
 *
 * See unix_socket.hpp.  The blocking path mirrors kernel/ipc/pipe.cpp +
 * tcp_socket.cpp: recv() parks on recv_waiters_ when the ring is empty (and the
 * peer has not closed / signalled EOF); accept() parks on accept_waiters_ when
 * no child is pending.  send() into the peer's ring + accept enqueue wake_one()
 * the relevant queue; close() wake_all()s both.  Host unit tests compile the
 * blocking out (CINUX_HOST_TEST); a non-blocking recv/accept returns WouldBlock.
 *
 * This file stays decoupled (CODING-TASTE §14 + check_net_decoupling.sh): it
 * pulls in no driver / dma / arch-irq header -- only the Socket base + the
 * scheduler wait-queue seam (host-guarded).
 *
 * Namespace: cinux::net
 */

#include "kernel/net/unix_socket.hpp"

#include <cstdint>

#ifndef CINUX_HOST_TEST
#    include "kernel/proc/process.hpp"    // Task::wait_next
#    include "kernel/proc/scheduler.hpp"  // prepare_to_wait/schedule_blocked/unblock
#endif

namespace cinux::net {

#ifndef CINUX_HOST_TEST
namespace {
using cinux::proc::Scheduler;
using cinux::proc::Task;

// Intrusive singly-linked wait queue helpers (identical to tcp_socket.cpp).  A
// blocked recv'er / accept'er parks on the queue; the producer wake_one()s it.
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

// ============================================================
// UnixRegistry -- in-memory path -> listening-socket map
// ============================================================

UnixRegistry& UnixRegistry::instance() {
    static UnixRegistry reg;
    return reg;
}

int UnixRegistry::find_locked(const char* path) const {
    if (path == nullptr) {
        return -1;
    }
    for (uint32_t i = 0; i < kUnixRegistryMax; ++i) {
        if (!entries_[i].used) {
            continue;
        }
        const char* a = entries_[i].path;
        uint32_t    j = 0;
        while (a[j] != '\0' && path[j] != '\0') {
            if (a[j] != path[j]) {
                break;
            }
            ++j;
        }
        if (a[j] == '\0' && path[j] == '\0') {
            return static_cast<int>(i);
        }
    }
    return -1;
}

cinux::lib::ErrorOr<void> UnixRegistry::register_listener(const char* path, UnixSocket* sock) {
    if (path == nullptr || path[0] == '\0' || sock == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    if (find_locked(path) >= 0) {
        return cinux::lib::Error::AlreadyExists;
    }
    for (uint32_t i = 0; i < kUnixRegistryMax; ++i) {
        if (!entries_[i].used) {
            uint32_t j = 0;
            while (j + 1 < kUnixPathMax && path[j] != '\0') {
                entries_[i].path[j] = path[j];
                ++j;
            }
            entries_[i].path[j] = '\0';
            entries_[i].used    = true;
            entries_[i].sock    = sock;
            return {};
        }
    }
    return cinux::lib::Error::OutOfMemory;  // table full
}

cinux::lib::ErrorOr<UnixSocket*> UnixRegistry::lookup(const char* path) {
    if (path == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    int  i = find_locked(path);
    if (i < 0) {
        return cinux::lib::Error::NotFound;
    }
    return entries_[i].sock;
}

void UnixRegistry::unregister(const char* path) {
    auto g = lock_.guard();
    int  i = find_locked(path);
    if (i < 0) {
        return;
    }
    entries_[i].used    = false;
    entries_[i].path[0] = '\0';
    entries_[i].sock    = nullptr;
}

// ============================================================
// UnixSocket
// ============================================================

UnixSocket::UnixSocket(int type) : Socket(kAfUnix, type) {}

cinux::lib::ErrorOr<void> UnixSocket::bind_path(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return cinux::lib::Error::InvalidArgument;
    }
    {
        auto g = lock_.irq_guard();
        if (bound_ || listening_ || connected_) {
            return cinux::lib::Error::InvalidArgument;
        }
    }
    // Register under the REGISTRY lock only (no socket lock held) -> no AB-BA
    // with connect_path, which takes registry-then-socket.
    auto r = UnixRegistry::instance().register_listener(path, this);
    if (!r.ok()) {
        return r.error();  // AlreadyExists / OutOfMemory
    }
    auto     g = lock_.irq_guard();
    // Copy the leaf name so close() can unregister it (bounded, NUL-terminated).
    uint32_t i = 0;
    while (i + 1 < kUnixPathMax && path[i] != '\0') {
        path_[i] = path[i];
        ++i;
    }
    path_[i] = '\0';
    bound_   = true;
    return {};
}

cinux::lib::ErrorOr<void> UnixSocket::connect_path(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return cinux::lib::Error::InvalidArgument;
    }
    // Look up the server under the registry lock ALONE (no socket lock held).
    UnixSocket* server = nullptr;
    {
        auto r = UnixRegistry::instance().lookup(path);
        if (!r.ok()) {
            return r.error();  // NotFound -> ENOENT
        }
        server = *r;
    }
    if (server == nullptr) {
        return cinux::lib::Error::NotFound;
    }
    {
        auto g = lock_.irq_guard();
        if (bound_ || listening_ || connected_) {
            return cinux::lib::Error::InvalidArgument;
        }
    }
    // Create the server-side child end of this connection and wire peers
    // (write-once). Each socket is locked on its own -- never two at once.
    UnixSocket* child = new UnixSocket(type_);
    if (child == nullptr) {
        return cinux::lib::Error::OutOfMemory;
    }
    {
        auto g     = lock_.irq_guard();
        connected_ = true;
        peer_      = child;
    }
    {
        auto g            = child->lock_.irq_guard();
        child->connected_ = true;
        child->peer_      = this;
    }
    // Hand the child to the server's accept queue (the server locks itself).
    if (!server->enqueue_accept(child)) {
        // Not listening / queue full -> connection refused. Unwind the wiring.
        auto g            = lock_.irq_guard();
        connected_        = false;
        peer_             = nullptr;
        auto g2           = child->lock_.irq_guard();
        child->connected_ = false;
        child->peer_      = nullptr;
        delete child;
        return cinux::lib::Error::ConnectionRefused;
    }
    return {};
}

cinux::lib::ErrorOr<void> UnixSocket::listen(int /*backlog*/) {
    auto g = lock_.irq_guard();
    if (!bound_ || listening_ || connected_) {
        return cinux::lib::Error::InvalidArgument;
    }
    // No protocol module to register with (unlike TCP): the name is already in
    // UnixRegistry from bind_path, and connect_path reaches the server through
    // it.  listen() just marks this socket willing to accept.
    listening_ = true;
    return {};
}

bool UnixSocket::enqueue_accept(UnixSocket* child) {
    auto g = lock_.irq_guard();
    if (!listening_ || accept_count_ >= kAcceptMax) {
        return false;  // not listening / queue full -> refuse the connect
    }
    accept_queue_[accept_tail_] = child;
    accept_tail_                = (accept_tail_ + 1) % kAcceptMax;
    ++accept_count_;
#ifndef CINUX_HOST_TEST
    wake_one(accept_waiters_);
#endif
    return true;
}

cinux::lib::ErrorOr<Socket*> UnixSocket::accept(Ipv4Addr* /*out_remote*/, uint16_t* /*out_port*/) {
    for (;;) {
#ifndef CINUX_HOST_TEST
        bool need_block = false;
#endif
        {
            auto g = lock_.irq_guard();
            if (accept_count_ > 0) {
                UnixSocket* child = accept_queue_[accept_head_];
                accept_head_      = (accept_head_ + 1) % kAcceptMax;
                --accept_count_;
                // out_remote/out_port are meaningless for AF_UNIX (no addr/port);
                // sys_accept skips fill_sockaddr_in for AF_UNIX, so leave them be.
                return static_cast<Socket*>(child);
            }
            if (closed_) {
                return cinux::lib::Error::InvalidArgument;  // listening socket closed
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
#endif
    }
}

cinux::lib::ErrorOr<int64_t> UnixSocket::send(const uint8_t* buf, uint32_t len) {
    if (buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    // SHUT_WR (shutdown(SHUT_WR)) recorded: further sends -> EPIPE-shaped.
    if (shut_write()) {
        return cinux::lib::Error::BrokenPipe;
    }
    UnixSocket* peer = nullptr;
    {
        auto g = lock_.irq_guard();
        if (!connected_ || closed_) {
            return cinux::lib::Error::InvalidArgument;  // ENOTCONN-shaped (mirrors TcpSocket)
        }
        peer = peer_;  // snapshot (write-once after connect/accept)
    }
    if (peer == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    // Copy into the PEER's RX ring under the peer's lock ONLY (no nested lock).
    uint32_t want = len > kRxSize ? kRxSize : len;
    auto     g    = peer->lock_.irq_guard();
    if (peer->closed_) {
        return cinux::lib::Error::BrokenPipe;  // peer closed -> EPIPE / SIGPIPE-shaped
    }
    uint32_t space = kRxSize - static_cast<uint32_t>(peer->rx_.size());
    if (space == 0) {
        // Ring full: a blocking send would park on a write-wait queue, but the
        // single-thread test never fills a 4 KB ring with a tiny message.  True
        // send-side flow control (block until the peer drains) is a follow-up;
        // for now report EAGAIN so a looping caller backpressures itself.
        return cinux::lib::Error::WouldBlock;
    }
    uint32_t n = want < space ? want : space;
    peer->rx_.push_batch(buf, n);
#ifndef CINUX_HOST_TEST
    wake_one(peer->recv_waiters_);
#endif
    return static_cast<int64_t>(n);
}

cinux::lib::ErrorOr<int64_t> UnixSocket::recv(uint8_t* buf, uint32_t len, Ipv4Addr* /*out_src*/,
                                              uint16_t* /*out_port*/) {
    if (buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    // SHUT_RD (shutdown(SHUT_RD)) recorded: further recvs return 0 (EOF).
    if (shut_read()) {
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
                // out_src/out_port are meaningless for AF_UNIX; callers pass nullptr.
                return static_cast<int64_t>(got);
            }
            if (peer_eof_) {
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
#endif
    }
}

void UnixSocket::close() {
    UnixSocket* peer = nullptr;
    {
        auto g = lock_.irq_guard();
        if (closed_) {
            return;  // idempotent
        }
        closed_ = true;
        peer    = peer_;  // snapshot (write-once)
    }
    // Tell the peer its writes should now EPIPE and its reads hit EOF.
    if (peer != nullptr) {
        auto g          = peer->lock_.irq_guard();
        peer->peer_eof_ = true;
#ifndef CINUX_HOST_TEST
        wake_all(peer->recv_waiters_);  // blocked recv'ers retry -> drained ring -> EOF
#endif
    }
    bool was_listening = false;
    {
        auto g        = lock_.irq_guard();
        was_listening = listening_;
        listening_    = false;
#ifndef CINUX_HOST_TEST
        wake_all(recv_waiters_);
        wake_all(accept_waiters_);  // blocked accept'ers retry -> empty queue -> err
#endif
    }
    // A listening socket releases its name so a later bind() can reuse it.
    if (was_listening && bound_) {
        UnixRegistry::instance().unregister(path_);
        bound_ = false;
    }
}

// ============================================================
// F-ECO batch 7b: address retrieval + socketpair peer wiring
// ============================================================

bool UnixSocket::get_local_addr(SockAddrStorage* out) const {
    if (out == nullptr) {
        return false;
    }
    auto g = lock_.irq_guard();
    if (!bound_) {
        return false;  // unnamed / unbound -> getsockname returns ENOTCONN-shaped
    }
    SockAddrUn sa{};
    sa.family = kAfUnix;
    // path_ is already NUL-terminated and bounded at kUnixPathMax; copy all 108
    // bytes so the user buffer matches a full sockaddr_un layout.
    for (uint32_t i = 0; i < kUnixPathMax; ++i) {
        sa.path[i] = path_[i];
    }
    __builtin_memcpy(out->bytes, &sa, sizeof(SockAddrUn));
    return true;
}

bool UnixSocket::get_peer_addr(SockAddrStorage* out) const {
    if (out == nullptr) {
        return false;
    }
    auto g = lock_.irq_guard();
    if (peer_ == nullptr) {
        return false;  // not connected -> getpeername returns ENOTCONN-shaped
    }
    // The peer of a connect_path/socketpair end is anonymous (an unbound
    // UnixSocket); Linux fills an empty-path sockaddr_un for such a peer.
    SockAddrUn sa{};
    sa.family = kAfUnix;
    // path[] already zero-initialised (anonymous).
    __builtin_memcpy(out->bytes, &sa, sizeof(SockAddrUn));
    return true;
}

void UnixSocket::pair_with(UnixSocket* other) {
    if (other == nullptr) {
        return;
    }
    // Lock each socket on its own -- NEVER two at once (mirrors connect_path).
    {
        auto g     = lock_.irq_guard();
        connected_ = true;
        peer_      = other;
    }
    {
        auto g            = other->lock_.irq_guard();
        other->connected_ = true;
        other->peer_      = this;
    }
}

}  // namespace cinux::net
