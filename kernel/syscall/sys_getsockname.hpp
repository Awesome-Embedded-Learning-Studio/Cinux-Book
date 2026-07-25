/**
 * @file kernel/syscall/sys_getsockname.hpp
 * @brief sys_getsockname handler declaration (F-ECO batch 7b)
 *
 * Retrieve the LOCAL address bound to a socket (what bind() set).  Delegates to
 * Socket::get_local_addr, which each subclass overrides from its bound state
 * (UnixSocket::path_ / UdpSocket::local_port_ / TcpSocket::local_port_).  An
 * unnamed socket (unbound, or AF_UNIX with no path) -> -EOPNOTSUPP.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// getsockname(fd, addr, addrlen) -- fill addr with the local socket address.
int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t, uint64_t,
                        uint64_t);

}  // namespace cinux::syscall
