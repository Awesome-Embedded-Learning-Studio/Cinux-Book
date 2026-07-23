/**
 * @file kernel/syscall/sys_getsockopt.cpp
 * @brief sys_getsockopt handler (F-ECO batch 7a)
 *
 * See sys_getsockopt.hpp.  do_getsockopt_kernel resolves the fd and writes a
 * kernel int; sys_getsockopt stages it out to the user optval + updates optlen.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_getsockopt.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user (SMAP/extable)
#include "kernel/errno.hpp"
#include "kernel/net/socket.hpp"          // Socket::type()
#include "kernel/syscall/sys_socket.hpp"  // socket_from_fd

namespace cinux::syscall {

using cinux::net::Socket;

namespace {
constexpr uint64_t kSolSocket = 1;  ///< SOL_SOCKET
constexpr uint64_t kSoType    = 3;  ///< SO_TYPE
}  // namespace

int64_t do_getsockopt_kernel(uint64_t fd, uint64_t level, uint64_t optname, int32_t* out_value) {
    Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    if (out_value == nullptr) {
        return -cinux::kEinval;
    }
    if (level != kSolSocket) {
        return -cinux::kEopnotsupp;  // IPPROTO_TCP/IPPROTO_IP options unsupported
    }
    int32_t v = 0;
    if (optname == kSoType) {
        v = static_cast<int32_t>(s->type());  // SOCK_STREAM(1) / SOCK_DGRAM(2)
    }
    // SO_ERROR / SO_ACCEPTCONN / other options -> 0 (no storage yet; follow-up).
    *out_value = v;
    return 0;
}

int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval,
                       uint64_t optlen_ptr, uint64_t) {
    int32_t v  = 0;
    int64_t rc = do_getsockopt_kernel(fd, level, optname, &v);
    if (rc < 0) {
        return rc;
    }
    if (optval != 0) {
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(optval), &v, sizeof(v))) {
            return -cinux::kEfault;
        }
    }
    if (optlen_ptr != 0) {
        uint32_t outlen = sizeof(v);  // socklen_t
        cinux::user::copy_to_user(reinterpret_cast<void*>(optlen_ptr), &outlen,
                                  sizeof(outlen));  // best-effort
    }
    return 0;
}

}  // namespace cinux::syscall
