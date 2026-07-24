/**
 * @file kernel/syscall/sys_select.hpp
 * @brief sys_select handler declaration (F8-M5)
 *
 * select(nfds, readfds, writefds, exceptfds, timeout): block until one of the
 * watched fds is ready or @p timeout elapses.  Implemented by translating the
 * fd_set bitmaps into a pollfd[] and running the SAME do_poll_core() engine as
 * sys_poll, so both share one blocking path.  busybox nc/wget lean on select.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// select(nfds, readfds, writefds, exceptfds, timeout) -- over do_poll_core.
int64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds,
                   uint64_t timeout, uint64_t);

}  // namespace cinux::syscall
