/**
 * @file kernel/syscall/sys_getpeername.cpp
 * @brief sys_getpeername handler (F-ECO batch 7b)
 *
 * Mirror of sys_getsockname over Socket::get_peer_addr.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_getpeername.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user (SMAP/extable)
#include "kernel/errno.hpp"
#include "kernel/net/socket.hpp"          // Socket / SockAddrStorage / SockAddrIn/Un / kAf*
#include "kernel/syscall/sys_socket.hpp"  // socket_from_fd

namespace cinux::syscall {

int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t, uint64_t,
                        uint64_t) {
    cinux::net::Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    cinux::net::SockAddrStorage st;
    if (!s->get_peer_addr(&st)) {
        return -cinux::kEopnotsupp;  // not connected: no peer address
    }
    if (addr == 0) {
        return 0;
    }
    uint32_t len = (s->domain() == cinux::net::kAfUnix) ? sizeof(cinux::net::SockAddrUn)
                                                        : sizeof(cinux::net::SockAddrIn);
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(addr), st.bytes, len)) {
        return -cinux::kEfault;
    }
    if (addrlen_ptr != 0) {
        uint32_t outlen = len;
        cinux::user::copy_to_user(reinterpret_cast<void*>(addrlen_ptr), &outlen, sizeof(outlen));
    }
    return 0;
}

}  // namespace cinux::syscall
