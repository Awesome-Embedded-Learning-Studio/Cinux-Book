/**
 * @file kernel/syscall/sys_shutdown.hpp
 * @brief sys_shutdown handler declaration (F-ECO batch 7b)
 *
 * shut down part of a full-duplex connection.  how = SHUT_RD (0) -> further recv
 * returns EOF; SHUT_WR (1) -> further send returns EPIPE; SHUT_RDWR (2) -> both.
 * Records the direction bits in the Socket base (do_shutdown); each subclass's
 * send/recv check shut_write()/shut_read() at entry.  Does NOT signal the peer
 * (no TCP FIN / AF_UNIX peer-EOF propagation in the hobby model -- follow-up).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// shutdown(fd, how) -- disable send / recv / both.  Returns 0 or -errno.
int64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
