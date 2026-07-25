/**
 * @file kernel/syscall/sys_socketpair.hpp
 * @brief socketpair(2) syscall handler (F-ECO batch 7b).
 *
 * socketpair() creates a pair of ALREADY-CONNECTED AF_UNIX sockets and returns
 * their fds in sv[2] (Linux syscall 53).  Both ends are anonymous (no bind());
 * they are wired as peers via UnixSocket::pair_with, so a write on one is
 * readable on the other immediately -- the canonical bidirectional IPC pipe
 * that shell syntax (`cmd1 |& cmd2`, or musl's libc tests) relies on.
 *
 * Two entry points, mirroring the accept()/do_accept() split:
 *   - do_socketpair_kernel(): kernel-pointer variant (sv is a real int[2] in
 *     kernel memory).  Tests call this directly to skip the user boundary.
 *   - sys_socketpair():       the user boundary (sv is a user pointer); calls
 *     do_socketpair_kernel then copy_to_user the two fds.
 *
 * Numbers are Linux x86_64 (socketpair = 53).  Namespace: cinux::syscall.
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"  // 6-arg syscall ABI

namespace cinux::syscall {

/// socketpair(2) user-boundary entry.  @p sv_virt is a user pointer to int[2].
/// Returns 0 on success (sv filled) or -errno.  See socketpair(2).
int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv_virt,
                       uint64_t, uint64_t);

/// socketpair(2) kernel-pointer core (tests call this; sys_socketpair wraps it
/// with copy_to_user).  On success fills @p sv_kernel[0..1] with the two fds.
/// @return 0 on success, -errno on failure (no fds installed on failure).
int64_t do_socketpair_kernel(uint64_t domain, uint64_t type, int* sv_kernel);

}  // namespace cinux::syscall
