/**
 * @file kernel/syscall/sys_accept4.cpp
 * @brief sys_accept4 handler (F-ECO batch 7a)
 *
 * Thin wrapper over do_accept (sys_socket.cpp), forwarding the flags word so
 * SOCK_CLOEXEC cloexec's the accepted fd.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_accept4.hpp"

#include <stdint.h>

#include "kernel/syscall/sys_socket.hpp"  // do_accept

namespace cinux::syscall {

int64_t sys_accept4(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t flags, uint64_t,
                    uint64_t) {
    return do_accept(fd, addr, addrlen_ptr, flags);
}

}  // namespace cinux::syscall
