/**
 * @file kernel/syscall/sys_setsockopt.hpp
 * @brief sys_setsockopt handler declaration (F-ECO batch 7a)
 *
 * Hobby-OS alignment: there is NO socket-option storage yet, so every option is
 * accepted as a no-op (return 0 for a valid socket fd).  Apps that set
 * SO_REUSEADDR / SO_RCVBUF / SO_KEEPALIVE / TCP_NODELAY / ... simply see them
 * take no effect rather than fail -- they almost never need the effect at hobby
 * scale (loopback, no addr-reuse conflict, no flow control).  Faithful option
 * semantics is a follow-up (a Socket options table + plumbing into the layers).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// setsockopt(fd, level, optname, optval, optlen) -- accept any option no-op.
int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval,
                       uint64_t optlen, uint64_t);

}  // namespace cinux::syscall
