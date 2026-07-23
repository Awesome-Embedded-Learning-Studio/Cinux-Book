/**
 * @file kernel/net/unix_socket.hpp
 * @brief AF_UNIX socket + in-memory name registry (F8-M3).
 *
 * AF_UNIX gives two tasks a byte-stream IPC channel addressed by a
 * filesystem-style path -- but the path lives in an IN-MEMORY namespace, not on
 * tmpfs/ext2.  A real filesystem-backed bind() (create/unlink a socket inode on
 * a mounted fs) is a documented follow-up; this milestone keeps the kernel off a
 * filesystem dependency (hobby-OS simplification, mirroring ipc::FifoRegistry).
 *
 * UnixSocket reuses the F7-M6 Socket base + SocketOps InodeOps shim: an AF_UNIX
 * socket fd is File -> Inode -> SocketOps exactly like a TCP/pipe fd, so
 * sys_read/write/close dispatch with zero fd-layer changes.  Two roles, one
 * class (mirrors TcpSocket):
 *
 *   - Listening (server): bind_path() registers with UnixRegistry; listen()
 *     marks it listening; accept() dequeues a connected child.
 *   - Connected (client after connect_path(), or an accepted child): holds a
 *     peer pointer; send() copies bytes into the PEER's RX ring, recv() drains
 *     its OWN.  Blocking mirrors TcpSocket/pipe (prepare_to_wait +
 *     schedule_blocked; NO sti/hlt -- the sti-in-syscall #DF hazard).
 *
 * connect_path() creates a child socket C, wires client<->C as peers, and
 * enqueues C onto the server's accept queue.  The connection is established
 * immediately (no in-kernel handshake), so a deterministic single-threaded test
 * can send BEFORE the server accepts -- the bytes buffer in C's RX ring until
 * accept() pulls C off.  This is faithful enough for loopback and keeps the
 * kernel off a timer-driven connect-timeout (a follow-up).
 *
 * LOCKING: each socket guards its own state with its lock_ (irq_guard, closing
 * the lost-wakeup window vs a wake from another CPU, like TcpSocket).  The
 * registry has its own lock and is NEVER held while a socket lock is held (no
 * AB-BA: bind_path/connect_path take the registry lock alone, then release it
 * before touching any socket).  peer_ is write-once (set at connect/accept), so
 * send() snapshots it under the local lock, releases, then acquires the peer's
 * lock alone -- never two socket locks at once.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cinux/ring_buffer.hpp>
#include <cstdint>

#include "kernel/net/socket.hpp"  // Socket
#include "kernel/proc/sync.hpp"   // Spinlock

namespace cinux::proc {
struct Task;  // forward -- wait queues hold blocked recv'ers / accept'ers
}

namespace cinux::net {

/// Maximum concurrently bound AF_UNIX names (fixed table; no <map>/<string>).
/// (kUnixPathMax -- the sockaddr_un path length -- lives in socket.hpp next to
/// SockAddrUn so the syscall handler + tests share one source of truth.)
static constexpr uint32_t kUnixRegistryMax = 16;

class UnixSocket;

/**
 * @brief In-memory path -> listening-socket registry (F8-M3)
 *
 * AF_UNIX names live here, NOT on a filesystem.  bind_path() registers a
 * listening UnixSocket under a leaf path; connect_path() looks it up to reach
 * the server; close() of a listening socket unregisters.  Mirrors
 * ipc::FifoRegistry (fixed table + Spinlock + leaf-name compare).  The registry
 * lock is process-wide and is NEVER nested inside a socket lock.
 */
class UnixRegistry {
public:
    /// Process-wide singleton (the kernel has one AF_UNIX namespace).
    static UnixRegistry& instance();

    /// Register @p sock as the listener for @p path.
    /// AlreadyExists if the path is taken, OutOfMemory if the table is full.
    cinux::lib::ErrorOr<void>        register_listener(const char* path, UnixSocket* sock);
    /// Look up the listening socket bound to @p path.  NotFound if absent.
    cinux::lib::ErrorOr<UnixSocket*> lookup(const char* path);
    /// Unregister @p path (on close of its listener).  No-op if absent.
    void                             unregister(const char* path);

private:
    UnixRegistry() = default;

    struct Entry {
        char        path[kUnixPathMax];
        bool        used{false};
        UnixSocket* sock{nullptr};
    };

    Entry                 entries_[kUnixRegistryMax];
    cinux::proc::Spinlock lock_;

    /// Index of @p path, or -1.  Caller holds lock_.
    int find_locked(const char* path) const;
};

/**
 * @brief AF_UNIX socket: byte-stream IPC behind a Socket fd (F8-M3).
 *
 * See file header for the listening/connected roles + locking rules.  This
 * class stays decoupled (CODING-TASTE §14 + check_net_decoupling.sh): it pulls
 * in no driver / dma / arch-irq header -- only the Socket base + the scheduler
 * wait-queue seam (host-guarded, mirroring tcp_socket.cpp).
 */
class UnixSocket : public Socket {
public:
    /// @brief Construct an unconnected AF_UNIX socket.
    /// @p type is kSockStream or kSockDgram (both accepted at socket() time;
    /// listen/accept/connect are stream-shaped -- DGRAM sendto(path) is a
    /// documented follow-up).
    explicit UnixSocket(int type);

    // --- AF_UNIX path overrides (Socket base) ---
    cinux::lib::ErrorOr<void> bind_path(const char* path) override;
    cinux::lib::ErrorOr<void> connect_path(const char* path) override;

    // --- reused Socket virtuals (family-agnostic) ---
    cinux::lib::ErrorOr<void>    listen(int backlog) override;
    cinux::lib::ErrorOr<Socket*> accept(Ipv4Addr* out_remote, uint16_t* out_port) override;
    cinux::lib::ErrorOr<int64_t> send(const uint8_t* buf, uint32_t len) override;
    cinux::lib::ErrorOr<int64_t> recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                      uint16_t* out_port) override;
    void                         close() override;

    // --- F-ECO batch 7b: address retrieval (getsockname/getpeername) ---
    // getsockname returns the bound path (or false if unbound/unnamed);
    // getpeername returns an anonymous AF_UNIX sockaddr (Linux fills an empty
    // path for an unbound peer) once connected, else false.
    bool get_local_addr(SockAddrStorage* out) const override;
    bool get_peer_addr(SockAddrStorage* out) const override;

    /// Wire TWO unconnected UnixSockets as each other's peer (socketpair(2)).
    /// Sets connected_ on both ends under each socket's own lock (NEVER both at
    /// once) -- the connect_path peer wiring minus registry/accept-queue.  Public
    /// so the socketpair syscall handler can reach across to both ends.
    void pair_with(UnixSocket* other);

    /// Called by a client's connect_path() on the SERVER: enqueue a connected
    /// child for a later accept() + wake a blocked accept'er.  Public because
    /// connect_path runs on the client and must reach across to the server.
    /// @return false (connection refused) if not listening or the queue is full.
    bool enqueue_accept(UnixSocket* child);

private:
    static constexpr uint32_t kRxSize    = 4096;  ///< per-connection byte-stream ring
    static constexpr uint32_t kAcceptMax = 4;     ///< pending-accept queue depth

    char path_[kUnixPathMax]{};  ///< bound leaf name (listener; for unregister)
    bool bound_     = false;     ///< bind_path succeeded
    bool listening_ = false;     ///< listen() succeeded
    bool connected_ = false;     ///< connected (client after connect / accepted child)
    bool closed_    = false;     ///< this end called close()
    bool peer_eof_  = false;     ///< the peer called close() -> recv EOF once drained

    /// The other end.  Write-once: set by connect_path()/accept() wiring and
    /// never mutated afterwards, so it can be snapshotted under lock_ then used
    /// without holding it.
    UnixSocket* peer_ = nullptr;

    /// Listening role: connected children pending accept().
    UnixSocket* accept_queue_[kAcceptMax]{};
    uint32_t    accept_head_ = 0, accept_tail_ = 0, accept_count_ = 0;

    /// Connected role: inbound byte stream (peer's send pushes, recv drains).
    cinux::lib::RingBuffer<uint8_t, kRxSize> rx_;

    // mutable: const address getters (get_local_addr/get_peer_addr) lock to take
    // a consistent snapshot of bound_/path_/peer_ (the rest of the API is non-const).
    mutable cinux::proc::Spinlock lock_;
    cinux::proc::Task*            recv_waiters_   = nullptr;  ///< blocked in recv()
    cinux::proc::Task*            accept_waiters_ = nullptr;  ///< blocked in accept()
};

}  // namespace cinux::net
