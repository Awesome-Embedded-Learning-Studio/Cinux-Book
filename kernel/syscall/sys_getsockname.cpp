/**
 * @file kernel/syscall/sys_getsockname.cpp
 * @brief sys_getsockname handler (F-ECO batch 7b)
 *
 * See sys_getsockname.hpp.  Asks Socket::get_local_addr to fill a SockAddrStorage
 * with a complete sockaddr (family at byte 0), then copies sizeof(sockaddr_in)
 * or sizeof(sockaddr_un) bytes to user -- the size picked from Socket::domain().
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_getsockname.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user (SMAP/extable)
#include "kernel/errno.hpp"
#include "kernel/net/socket.hpp"          // Socket / SockAddrStorage / SockAddrIn/Un / kAf*
#include "kernel/syscall/sys_socket.hpp"  // socket_from_fd

namespace cinux::syscall {

int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t, uint64_t,
                        uint64_t) {
    cinux::net::Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    cinux::net::SockAddrStorage st;
    if (!s->get_local_addr(&st)) {
        return -cinux::kEopnotsupp;  // unnamed: not bound / no local address
    }
    if (addr == 0) {
        return 0;  // caller does not want the address (Linux allows NULL addr)
    }
    uint32_t len = (s->domain() == cinux::net::kAfUnix) ? sizeof(cinux::net::SockAddrUn)
                                                        : sizeof(cinux::net::SockAddrIn);
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(addr), st.bytes, len)) {
        return -cinux::kEfault;
    }
    if (addrlen_ptr != 0) {
        uint32_t outlen = len;  // socklen_t
        cinux::user::copy_to_user(reinterpret_cast<void*>(addrlen_ptr), &outlen, sizeof(outlen));
    }
    return 0;
}

}  // namespace cinux::syscall
