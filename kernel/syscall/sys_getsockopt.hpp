/**
 * @file kernel/syscall/sys_getsockopt.hpp
 * @brief sys_getsockopt handler declaration (F-ECO batch 7a)
 *
 * Returns the few options a libc / network app probes at startup: SO_TYPE (the
 * socket type) and SO_ERROR (0 -- no pending error tracked).  Other SOL_SOCKET
 * options report 0; non-SOL_SOCKET levels -> -EOPNOTSUPP.  Like setsockopt,
 * there is no socket-option STORAGE yet, so options that have real values
 * (SO_RCVBUF / SO_ACCEPTCONN / ...) are deferred to the options-table follow-up.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// getsockopt(fd, level, optname, optval, optlen) -- fill optval; returns 0.
int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval,
                       uint64_t optlen_ptr, uint64_t);

/// Tests call this; sys_getsockopt is the user boundary (copy_to_user).
/// Writes the option's int value into @p out_value (4 bytes).
int64_t do_getsockopt_kernel(uint64_t fd, uint64_t level, uint64_t optname, int32_t* out_value);

}  // namespace cinux::syscall
