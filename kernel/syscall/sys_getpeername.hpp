/**
 * @file kernel/syscall/sys_getpeername.hpp
 * @brief sys_getpeername handler declaration (F-ECO batch 7b)
 *
 * Retrieve the PEER address (who the socket connected to / accepted).  Delegates
 * to Socket::get_peer_addr.  An unconnected socket -> -ENOTCONN (returned as
 * get_peer_addr == false -> -EOPNOTSUPP here, the hobby-OS mapping).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// getpeername(fd, addr, addrlen) -- fill addr with the peer socket address.
int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t, uint64_t,
                        uint64_t);

}  // namespace cinux::syscall
