/**
 * @file kernel/syscall/sys_accept4.hpp
 * @brief sys_accept4 handler declaration (F-ECO batch 7a)
 *
 * accept4 = accept + a flags word.  Only SOCK_CLOEXEC is honoured (cloexec's
 * the new fd); SOCK_NONBLOCK is accepted but not plumbed (no per-fd nonblock
 * flag yet).  Shares do_accept with sys_accept.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// accept4(fd, addr, addrlen, flags) -- accept + flags (SOCK_CLOEXEC / SOCK_NONBLOCK).
int64_t sys_accept4(uint64_t fd, uint64_t addr, uint64_t addrlen_ptr, uint64_t flags, uint64_t,
                    uint64_t);

}  // namespace cinux::syscall
