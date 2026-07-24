/**
 * @file kernel/net/socket.hpp
 * @brief Socket base + SocketOps (InodeOps shim) + sockaddr UAPI (F7-M6).
 *
 * The user-visible socket API rides on the EXISTING fd layer exactly like
 * PTY/pipe: a socket fd is File -> Inode -> SocketOps (an InodeOps subclass),
 * so sys_read/write/ioctl/close dispatch through ops with ZERO changes to the fd
 * layer.  ONE stateless SocketOps instance serves every socket inode; the
 * per-fd state (the Socket*) lives in inode->fs_private (the PTY pattern).
 *
 * Socket is a concrete base with stub virtuals (return NotImplemented): the fd
 * machinery is exercisable BEFORE any protocol adapter exists (batch 1b), and a
 * bare Socket answers socket()/close() while rejecting protocol ops.  B2 (UDP)
 * and B3 (TCP) subclass it -- UdpSocket/UdpListener and TcpSocket/TcpListener --
 * overriding bind/connect/listen/accept/send/recv and pushing inbound frames
 * (copied out of the borrowed on_udp/on_data payload) into a per-socket RX ring.
 *
 * The protocol modules (TcpModule/UdpModule, F7-M4/M5) stay PURE: blocking +
 * buffering live here, in the adapter, not in the FSM (CODING-TASTE §13).
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/fs/inode.hpp"       // InodeOps
#include "kernel/net/net_types.hpp"  // Ipv4Addr

namespace cinux::net {

// ============================================================
// UAPI constants (mirror Linux userspace values; musl compiles against these)
// ============================================================

constexpr int kAfUnix     = 1;  ///< AF_UNIX (local IPC) -- F8-M3
constexpr int kAfInet     = 2;  ///< AF_INET (IPv4)
constexpr int kSockStream = 1;  ///< TCP
constexpr int kSockDgram  = 2;  ///< UDP
constexpr int kSockRaw    = 3;  ///< RAW (IPPROTO_ICMP ping, like Linux SOCK_RAW)

/// IPv4 socket address -- kernel mirror of libc sockaddr_in (16 bytes).
/// @note port is NETWORK order on the wire (musl lays it out big-endian); the
///       syscall handler ntohs()s it into host order before handing it to a
///       Socket.  family is host order (musl writes 2).
struct SockAddrIn {
    uint16_t family;   ///< AF_INET (host order)
    uint16_t port;     ///< NETWORK order (big-endian)
    uint8_t  addr[4];  ///< IPv4 address (network byte order)
    uint8_t  zero[8];  ///< padding to sizeof(sockaddr) = 16
};
static_assert(sizeof(SockAddrIn) == 16, "sockaddr_in is 16 bytes");

/// AF_UNIX socket address -- kernel mirror of libc sockaddr_un.  @p path is a
/// NUL-terminated filesystem-style leaf name in an IN-MEMORY namespace (a real
/// tmpfs/ext2-backed bind() is a documented follow-up; this milestone keeps the
/// kernel off a filesystem dependency).  family is host order (musl writes 1).
/// @note 110 bytes is the Linux layout; libc sockaddr_un carries trailing pad to
///       128, which the syscall handler does NOT need (it reads family + path).
struct SockAddrUn {
    uint16_t family;     ///< AF_UNIX (host order)
    char     path[108];  ///< NUL-terminated leaf name
};
static_assert(sizeof(SockAddrUn) == 110, "sockaddr_un is family(2) + path[108]");

/// Length of the sockaddr_un path field (Linux sun_path).  Lives here, next to
/// SockAddrUn, so every translation unit that sees the struct also sees the
/// bound (the syscall handler + tests loop path bytes up to this length).
static constexpr uint32_t kUnixPathMax = 108;

/// Generic socket-address storage (F-ECO batch 7b).  getsockname/getpeername
/// ask the Socket to fill this with a COMPLETE sockaddr (sockaddr_in for AF_INET
/// or sockaddr_un for AF_UNIX -- family is the first 2 bytes either way).  The
/// syscall handler then copies sizeof(sockaddr_in)/sizeof(sockaddr_un) bytes to
/// user space, picking the size from Socket::domain().  112 bytes holds the
/// largest (sockaddr_un = 110).
struct SockAddrStorage {
    char bytes[112];  ///< a full sockaddr_in (16) or sockaddr_un (110)
};

/// shutdown(2) "how" values (Linux).
constexpr int kShutRd   = 0;  ///< further recv -> EOF
constexpr int kShutWr   = 1;  ///< further send -> EPIPE
constexpr int kShutRdwr = 2;  ///< both

class Socket;

/// @brief Socket base -- the per-fd connection state + protocol-agnostic API.
///
/// Default virtuals return Error::NotImplemented so batch 1b can land the fd +
/// syscall plumbing with no protocol adapter: socket() succeeds, close() works,
/// and bind/connect/send/recv answer "not implemented" (-ENOSYS) until B2/B3
/// override them.  Subclasses ALSO implement UdpListener / TcpListener so the
/// protocol modules push inbound bytes INTO the socket (the modules stay pure).
class Socket {
public:
    Socket(int domain, int type) : domain_(domain), type_(type) {}
    virtual ~Socket() = default;

    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;

    /// @name Protocol API (overridden by UdpSocket / TcpSocket in B2 / B3).
    ///@{
    virtual cinux::lib::ErrorOr<void> bind(uint16_t local_port);
    virtual cinux::lib::ErrorOr<void> connect(Ipv4Addr remote, uint16_t remote_port);

    /// @name AF_UNIX (path-based) API -- overridden by UnixSocket (F8-M3).
    /// AF_UNIX names are filesystem-style paths, which do NOT fit the
    /// AF_INET-shaped bind(port)/connect(addr,port) virtuals above.  These
    /// path-shaped variants default to NotImplemented (so an AF_INET socket
    /// answers -ENOSYS, never reaching here through sys_bind/sys_connect which
    /// dispatch on Socket::domain()); UnixSocket overrides both.  listen /
    /// accept / send / recv are family-agnostic and reuse the virtuals below.
    ///@{
    virtual cinux::lib::ErrorOr<void>    bind_path(const char* path);
    virtual cinux::lib::ErrorOr<void>    connect_path(const char* path);
    ///@}
    virtual cinux::lib::ErrorOr<void>    listen(int backlog);
    /// Passive accept: return a NEW Socket* for a completed connection (TCP).
    /// The caller (sys_accept) installs it under a fresh fd.
    virtual cinux::lib::ErrorOr<Socket*> accept(Ipv4Addr* out_remote, uint16_t* out_port);
    /// Send on a connected socket (TCP established, or UDP after connect()).
    virtual cinux::lib::ErrorOr<int64_t> send(const uint8_t* buf, uint32_t len);
    /// Send a datagram to an explicit destination (UDP sendto).
    virtual cinux::lib::ErrorOr<int64_t> sendto(Ipv4Addr dst, uint16_t dst_port, const uint8_t* buf,
                                                uint32_t len);
    /// Receive; on success fills @p out_src/@p out_port (pass nullptr to ignore).
    /// Blocks on a per-socket wait queue when no data is available (B2/B3).
    virtual cinux::lib::ErrorOr<int64_t> recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                              uint16_t* out_port);
    /// Release protocol resources (unbind / stop_listen / FIN).  No-op by default.
    virtual void                         close();
    ///@}

    /// @name F-ECO batch 7b: address retrieval (getsockname/getpeername) +
    /// shutdown.  Default get_*_addr return false (unnamed); subclasses with
    /// bound/peer state override.  do_shutdown records direction bits that the
    /// subclass send/recv check at entry (SHUT_WR -> send EPIPE, SHUT_RD -> EOF).
    ///@{
    virtual bool get_local_addr(SockAddrStorage* out) const;
    virtual bool get_peer_addr(SockAddrStorage* out) const;
    void         do_shutdown(int how);
    bool         shut_read() const { return (shut_ & kShutRdBit) != 0; }
    bool         shut_write() const { return (shut_ & kShutWrBit) != 0; }
    ///@}

    /// @name F8-M5 poll(2)/select(2) readiness + wait registration.
    /// Subclasses (Udp/Tcp/Unix) override to report their rx ring / accept queue
    /// and to park a poller on the relevant wait queue (the same queue a blocked
    /// recv/accept sleeps on, so an incoming byte / connection wakes both).
    /// Default: a bare unconnected socket reports nothing ready and never parks.
    ///@{
    virtual uint32_t poll_events(cinux::proc::Task* waiter, bool* registered);
    virtual void     poll_detach_waiter(cinux::proc::Task* waiter);
    ///@}

    int domain() const { return domain_; }  ///< AF_INET
    int type() const { return type_; }      ///< SOCK_STREAM / SOCK_DGRAM

protected:
    static constexpr uint8_t kShutRdBit = 1u;  ///< SHUT_RD recorded
    static constexpr uint8_t kShutWrBit = 2u;  ///< SHUT_WR recorded

    int     domain_;    ///< AF_INET
    int     type_;      ///< SOCK_STREAM / SOCK_DGRAM
    uint8_t shut_ = 0;  ///< shutdown direction bits (do_shutdown)
};

/// Typed accessor: a socket inode's fs_private holds its Socket.  Centralises
/// the downcast at the InodeOps/socket syscall seam.
inline Socket* socket_of(const cinux::fs::Inode* inode) {
    return static_cast<Socket*>(inode->fs_private);
}

/// @brief InodeOps shim -- makes a socket fd indistinguishable from a pipe fd to
///        the fd layer.  ONE shared stateless instance (socket_ops()); the
///        per-fd Socket lives in inode->fs_private.
///
/// read() -> Socket::recv, write() -> Socket::send.  The addr-aware operations
/// (bind/connect/listen/accept/sendto/recvfrom) go through the socket syscalls,
/// NOT read/write -- sys_read/sys_write are the byte-stream face a connected
/// socket shows to generic code (and to musl read()/write() on the fd).
class SocketOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> read(const cinux::fs::Inode* inode, uint64_t /*offset*/, void* buf,
                                      uint64_t count) override;
    cinux::lib::ErrorOr<int64_t> write(cinux::fs::Inode* inode, uint64_t /*offset*/,
                                       const void* buf, uint64_t count) override;
    // F8-M5 poll: delegate to the per-fd Socket's readiness + wait registration.
    uint32_t poll_events(const cinux::fs::Inode* inode, cinux::proc::Task* waiter,
                         bool* registered) override;
    void     poll_detach_waiter(const cinux::fs::Inode* inode, cinux::proc::Task* waiter) override;
    // F8-M5 release: closing a socket fd releases its protocol resources
    // (unbind / stop_listen / FIN).  One Socket per fd, so no refcounting needed.
    void     release(cinux::fs::Inode* inode) override;
    // ioctl/stat/open inherit the InodeOps defaults (NotImplemented/-1/false) --
    // a socket is not a tty, not a disk file, not a cloning device.
};

/// @brief The single shared SocketOps instance every socket inode points at.
SocketOps& socket_ops();

}  // namespace cinux::net
