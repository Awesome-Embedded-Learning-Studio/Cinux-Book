/**
 * @file kernel/syscall/sys_shutdown.cpp
 * @brief sys_shutdown handler (F-ECO batch 7b)
 *
 * See sys_shutdown.hpp.  Thin shim over Socket::do_shutdown (base state); the
 * subclass send/recv consult the recorded bits.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_shutdown.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/net/socket.hpp"          // Socket::do_shutdown + kShut* constants
#include "kernel/syscall/sys_socket.hpp"  // socket_from_fd

namespace cinux::syscall {

int64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::net::Socket* s = socket_from_fd(fd);
    if (s == nullptr) {
        return -cinux::kEbadf;
    }
    if (how > static_cast<uint64_t>(cinux::net::kShutRdwr)) {
        return -cinux::kEinval;  // SHUT_RD/WR/RDWR = 0/1/2
    }
    s->do_shutdown(static_cast<int>(how));
    return 0;
}

}  // namespace cinux::syscall
