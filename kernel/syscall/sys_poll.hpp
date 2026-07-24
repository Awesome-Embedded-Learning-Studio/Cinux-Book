/**
 * @file kernel/syscall/sys_poll.hpp
 * @brief sys_poll handler declaration (F8-M5 real poll)
 *
 * poll(fds, nfds, timeout): block until one of @p nfds fds is ready (readable /
 * writable / hung-up) or @p timeout ms elapse, returning the count of ready fds
 * and filling each pollfd's revents.  The blocking + wait-queue logic lives in
 * do_poll_core() (shared with sys_select); this handler just stages the pollfd
 * array across the user boundary.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// poll(fds, nfds, timeout) -- real blocking poll over do_poll_core.
int64_t sys_poll(uint64_t fds_virt, uint64_t nfds, uint64_t timeout, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
