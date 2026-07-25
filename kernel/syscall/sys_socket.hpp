/**
 * @file kernel/syscall/sys_socket.hpp
 * @brief BSD socket syscall handlers (F7-M6).
 *
 * Every handler is a thin user-boundary shim: resolve fd -> File -> inode ->
 * SocketOps -> Socket* (fs_private), call the matching Socket virtual, and cross
 * the user/kernel boundary for sockaddr_in + data buffers via copy_to/from_user
 * (SMAP/extable-safe, the same primitives sys_read/sys_ioctl use).  The Socket
 * base (batch 1b) answers these with Error::NotImplemented -> -ENOSYS until
 * B2 (UdpSocket) / B3 (TcpSocket) override the virtuals to do real work.
 *
 * Numbers are Linux x86_64 (see syscall_nums.hpp): socket=41, connect=42,
 * accept=43, sendto=44, recvfrom=45, bind=49, listen=50.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::net {
class Socket;  // forward -- see kernel/net/socket.hpp
}

namespace cinux::syscall {

int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t, uint64_t, uint64_t);
int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t, uint64_t, uint64_t);
int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t, uint64_t, uint64_t);
int64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t, uint64_t, uint64_t);
int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                   uint64_t addrlen);
int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                     uint64_t addrlen_ptr);

// --- shared helpers (F-ECO batch 7): reused by accept4 / setsockopt / getsockopt
// / shutdown / getsockname / getpeername / socketpair handlers. ---
//
/// Recover the Socket* behind @p fd, or nullptr if it is not a socket fd.
cinux::net::Socket* socket_from_fd(uint64_t fd);
/// Install @p sock under a fresh fd (SocketOps inode + FDTable::alloc RDWR).
/// Returns the fd or -errno.  Ownership of @p sock + the Inode transfers to the
/// FDTable's File on success; on failure both are freed.
int64_t             install_socket_fd(cinux::net::Socket* sock);
/// accept core shared by sys_accept (flags=0) and sys_accept4 (SOCK_CLOEXEC
/// etc.).  @p flags is the accept4 flag word; SOCK_CLOEXEC cloexec's the new fd.
int64_t             do_accept(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t flags);

}  // namespace cinux::syscall
