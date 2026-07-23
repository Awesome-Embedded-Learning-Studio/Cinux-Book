/**
 * @file kernel/syscall/sys_socket.cpp
 * @brief BSD socket syscall handlers (F7-M6).
 *
 * Thin user-boundary shims (see sys_socket.hpp).  fd -> File -> inode -> Socket
 * resolution mirrors sys_ioctl's fd>2 path (the ops pointer discriminates a
 * socket fd from a file/pipe/tty fd).  sockaddr_in crosses the boundary via
 * copy_to/from_user; data buffers stage through a heap kbuf so a large user
 * length never blows the kernel stack frame (-Wframe-larger-than=1024).
 *
 * In batch 1b the Socket base returns Error::NotImplemented for every protocol
 * op, so bind/connect/listen/accept/sendto/recvfrom answer -ENOSYS; socket()
 * itself + close() (via FDTable) already work, which is what the batch-1b kernel
 * test exercises.  B2 (UdpSocket) / B3 (TcpSocket) override the virtuals.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_socket.hpp"

#include <cstdint>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (SMAP/extable)
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"          // FDTable / File
#include "kernel/fs/vfs_mount.hpp"     // current_fd_table()
#include "kernel/net/net_init.hpp"     // create_socket
#include "kernel/net/net_types.hpp"    // Ipv4Addr
#include "kernel/net/socket.hpp"       // Socket / SocketOps / SockAddrIn / SockAddrUn
#include "kernel/net/unix_socket.hpp"  // UnixSocket (AF_UNIX, F8-M3)

namespace cinux::syscall {

using cinux::fs::FDTable;
using cinux::fs::File;
using cinux::fs::Inode;
using cinux::fs::InodeType;
using cinux::fs::OpenFlags;
using cinux::fs::current_fd_table;
using cinux::net::Ipv4Addr;
using cinux::net::Socket;
using cinux::net::SockAddrIn;
using cinux::net::SockAddrUn;
using cinux::net::UnixSocket;
using cinux::net::create_socket;
using cinux::net::kAfInet;
using cinux::net::kAfUnix;
using cinux::net::kSockDgram;
using cinux::net::kSockStream;
using cinux::net::socket_ops;
using cinux::user::copy_from_user;
using cinux::user::copy_to_user;

namespace {

/// Cap on a single sendto/recvfrom staging buffer.  UDP fits an Ethernet frame
/// (~1472); TCP sends in segments.  A page bounds the heap alloc without
/// touching the kernel stack.  Larger writes loop in a later batch.
constexpr uint32_t kMaxSockBuf = 4096;

/// Byte-swap a 16-bit value (sockaddr_in port is NETWORK order on the wire).
inline uint16_t byte_swap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

/// Parse a user sockaddr_in (addr + VALUE addrlen) into host-order addr+port.
/// @return true on a valid AF_INET address; false on bad ptr / short len / wrong family.
bool parse_sockaddr_in(uint64_t addr_virt, uint64_t addrlen, Ipv4Addr* out_addr,
                       uint16_t* out_port) {
    if (addr_virt == 0 || addrlen < sizeof(SockAddrIn)) {
        return false;
    }
    SockAddrIn sa;
    if (!copy_from_user(&sa, reinterpret_cast<void*>(addr_virt), sizeof(sa))) {
        return false;
    }
    if (sa.family != kAfInet) {
        return false;
    }
    *out_port = byte_swap16(sa.port);
    for (int i = 0; i < 4; ++i) {
        out_addr->oct[i] = sa.addr[i];
    }
    return true;
}

/// Write a host-order addr+port back into a user sockaddr_in (accept/recvfrom),
/// and update the in/out *addrlen.  A NULL @p addr_virt means the caller does
/// not want the address (Linux allows it) -- a silent no-op.
bool fill_sockaddr_in(uint64_t addr_virt, uint64_t addrlen_ptr, Ipv4Addr addr, uint16_t port) {
    if (addr_virt == 0) {
        return true;
    }
    SockAddrIn sa{};
    sa.family = kAfInet;
    sa.port   = byte_swap16(port);
    for (int i = 0; i < 4; ++i) {
        sa.addr[i] = addr.oct[i];
    }
    if (!copy_to_user(reinterpret_cast<void*>(addr_virt), &sa, sizeof(sa))) {
        return false;
    }
    if (addrlen_ptr != 0) {
        uint16_t len = sizeof(sa);
        copy_to_user(reinterpret_cast<void*>(addrlen_ptr), &len, sizeof(len));  // best-effort
    }
    return true;
}

/// Parse a user sockaddr_un into a NUL-terminated @p out_path (always written).
/// @return true on a valid AF_UNIX address; false on bad ptr / short len / wrong
/// family.  @p out_path must have room for kUnixPathMax chars.
bool parse_sockaddr_un(uint64_t addr_virt, uint64_t addrlen, char* out_path) {
    if (addr_virt == 0 || addrlen < 2) {  // need at least the family field
        return false;
    }
    SockAddrUn sa;
    if (!copy_from_user(&sa, reinterpret_cast<void*>(addr_virt), sizeof(sa))) {
        return false;
    }
    if (sa.family != kAfUnix) {
        return false;
    }
    // Force NUL-termination regardless of what userspace wrote.
    uint32_t i = 0;
    while (i + 1 < cinux::net::kUnixPathMax && sa.path[i] != '\0') {
        out_path[i] = sa.path[i];
        ++i;
    }
    out_path[i] = '\0';
    return out_path[0] != '\0';  // Linux allows empty (abstract) paths; we do not yet
}

}  // namespace

// ============================================================
// Shared helpers (F-ECO batch 7): exposed in sys_socket.hpp so the accept4 /
// setsockopt / getsockopt / shutdown / getsockname / getpeername / socketpair
// handlers reuse the SAME fd->Socket resolution + fd install, not a copy.
// Bodies are unchanged from their original anonymous-namespace definitions.
// ============================================================

Socket* socket_from_fd(uint64_t fd) {
    File* file = current_fd_table().get(static_cast<int>(fd));
    if (file == nullptr || file->inode == nullptr || file->inode->ops != &socket_ops()) {
        return nullptr;
    }
    return static_cast<Socket*>(file->inode->fs_private);
}

int64_t install_socket_fd(Socket* sock) {
    // Manual RAII (freestanding: no <memory>/unique_ptr). On error free both;
    // on success ownership of sock + inode transfers to the FDTable's File
    // (closing the fd frees the File; the Socket/Inode share the pipe-style
    // hobby-OS release-without-hook limitation).
    Socket* s         = sock;
    Inode*  inode     = new Inode();
    inode->ops        = &socket_ops();
    inode->type       = InodeType::Regular;
    inode->fs_private = s;

    int fd = current_fd_table().alloc(inode, OpenFlags::RDWR);
    if (fd < 0) {
        delete inode;
        delete s;
        return -cinux::kEmfile;
    }
    return fd;
}

int64_t do_accept(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t flags) {
    Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    Ipv4Addr remote{};
    uint16_t rport = 0;
    auto     r     = s->accept(&remote, &rport);
    if (!r.ok()) {
        return -cinux::to_errno(r.error());
    }
    int64_t new_fd = install_socket_fd(*r);
    if (new_fd < 0) {
        (*r)->close();  // free the accepted socket the fd table could not hold
        return new_fd;
    }
    // SOCK_CLOEXEC (Linux 02000000) -> mark the new fd close-on-exec.  SOCK_NONBLOCK
    // is accepted but not plumbed (no per-fd nonblock flag yet -- follow-up).
    constexpr uint64_t kSockCloexec = 02000000;
    if ((flags & kSockCloexec) != 0) {
        File* nf = current_fd_table().get(static_cast<int>(new_fd));
        if (nf != nullptr) {
            nf->cloexec = true;
        }
    }
    // AF_UNIX peers carry no address/port -- leave the caller's sockaddr untouched.
    if (s->domain() != kAfUnix) {
        fill_sockaddr_in(addr, addrlen_ptr, remote, rport);
    }
    return new_fd;
}

int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t /*protocol*/, uint64_t, uint64_t,
                   uint64_t) {
    if (type != static_cast<uint64_t>(kSockStream) && type != static_cast<uint64_t>(kSockDgram)) {
        return -cinux::kEprotonosupport;
    }
    // AF_UNIX is self-contained (no NIC / L4 module), so build it here directly
    // instead of going through create_socket() -- that factory is the ONE bridge
    // to the production L4 stack (AF_INET) and returns nullptr when the stack is
    // down.  Handling AF_UNIX here keeps it working in the test kernel (no
    // net_init) and avoids touching drivers/net + net_stub in lockstep.
    if (domain == static_cast<uint64_t>(kAfUnix)) {
        Socket* s = new UnixSocket(static_cast<int>(type));
        return install_socket_fd(s);
    }
    if (domain != static_cast<uint64_t>(kAfInet)) {
        return -cinux::kEafnosupport;
    }
    Socket* s = create_socket(static_cast<int>(domain), static_cast<int>(type));
    if (s == nullptr) {
        return -cinux::kEprotonosupport;  // stack not up (no NIC) / unsupported
    }
    return install_socket_fd(s);
}

int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t, uint64_t, uint64_t) {
    Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    if (s->domain() == kAfUnix) {
        char path[cinux::net::kUnixPathMax];
        if (!parse_sockaddr_un(addr, addrlen, path)) {
            return -cinux::kEfault;
        }
        auto r = s->bind_path(path);
        return r.ok() ? 0 : -cinux::to_errno(r.error());
    }
    Ipv4Addr a{};
    uint16_t port = 0;
    if (!parse_sockaddr_in(addr, addrlen, &a, &port)) {
        return -cinux::kEfault;
    }
    auto r = s->bind(port);
    return r.ok() ? 0 : -cinux::to_errno(r.error());
}

int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t, uint64_t, uint64_t) {
    Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    if (s->domain() == kAfUnix) {
        char path[cinux::net::kUnixPathMax];
        if (!parse_sockaddr_un(addr, addrlen, path)) {
            return -cinux::kEfault;
        }
        auto r = s->connect_path(path);
        return r.ok() ? 0 : -cinux::to_errno(r.error());
    }
    Ipv4Addr a{};
    uint16_t port = 0;
    if (!parse_sockaddr_in(addr, addrlen, &a, &port)) {
        return -cinux::kEfault;
    }
    auto r = s->connect(a, port);
    return r.ok() ? 0 : -cinux::to_errno(r.error());
}

int64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t, uint64_t, uint64_t, uint64_t) {
    Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    auto r = s->listen(static_cast<int>(backlog));
    return r.ok() ? 0 : -cinux::to_errno(r.error());
}

int64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t, uint64_t, uint64_t) {
    return do_accept(fd, addr, addrlen_ptr, 0);  // flags=0 (sys_accept has no flags)
}

int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t /*flags*/, uint64_t addr,
                   uint64_t addrlen) {
    Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    uint32_t n    = len > kMaxSockBuf ? kMaxSockBuf : static_cast<uint32_t>(len);
    uint8_t* kbuf = new uint8_t[n ? n : 1];  // manual RAII (freestanding: no <memory>)
    if (n != 0 && !copy_from_user(kbuf, reinterpret_cast<void*>(buf), n)) {
        delete[] kbuf;
        return -cinux::kEfault;
    }
    cinux::lib::ErrorOr<int64_t> r = (addr == 0) ? s->send(kbuf, n) : [&] {
        Ipv4Addr a{};
        uint16_t port = 0;
        return parse_sockaddr_in(addr, addrlen, &a, &port)
                   ? s->sendto(a, port, kbuf, n)
                   : cinux::lib::ErrorOr<int64_t>(cinux::lib::Error::InvalidArgument);
    }();
    delete[] kbuf;  // send/sendto consumed it
    if (!r.ok()) {
        return -cinux::to_errno(r.error());
    }
    return *r;
}

int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t /*flags*/, uint64_t addr,
                     uint64_t addrlen_ptr) {
    Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    uint32_t n    = len > kMaxSockBuf ? kMaxSockBuf : static_cast<uint32_t>(len);
    uint8_t* kbuf = new uint8_t[n ? n : 1];  // manual RAII (freestanding: no <memory>)
    Ipv4Addr src{};
    uint16_t sport = 0;
    auto     r     = s->recv(kbuf, n, &src, &sport);
    if (!r.ok()) {
        delete[] kbuf;
        return -cinux::to_errno(r.error());
    }
    uint32_t got = static_cast<uint32_t>(*r);
    if (got != 0 && !copy_to_user(reinterpret_cast<void*>(buf), kbuf, got)) {
        delete[] kbuf;
        return -cinux::kEfault;
    }
    delete[] kbuf;  // copied out to user
    fill_sockaddr_in(addr, addrlen_ptr, src, sport);
    return *r;
}

}  // namespace cinux::syscall
