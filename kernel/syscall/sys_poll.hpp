/**
 * @file kernel/syscall/sys_poll.hpp
 * @brief sys_poll handler declaration (F-ECO busybox sh smoke)
 *
 * **STUB**: reports every polled fd as ready (revents = events & POLLIN).  Real
 * poll -- blocking until an fd is ready or the timeout elapses, with wait-queue
 * plumbing -- is F8-M5 (epoll/poll).  This stub unblocks busybox `sh`'s input
 * loop (it poll()s stdin before read()), which then blocks in read() on the
 * console TTY until the user types -- so the smoke test is interactive even
 * without a real poll.  Always returns immediately (ignores @p timeout).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// poll(fds, nfds, timeout) -- stub: mark each fd POLLIN-ready. Returns nfds.
int64_t sys_poll(uint64_t fds_virt, uint64_t nfds, uint64_t /*timeout*/, uint64_t, uint64_t,
                 uint64_t);

}  // namespace cinux::syscall
