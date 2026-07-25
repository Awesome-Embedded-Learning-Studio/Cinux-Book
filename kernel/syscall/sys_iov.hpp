/**
 * @file kernel/syscall/sys_iov.hpp
 * @brief sys_writev / sys_readv handler declarations (F10-M1 batch 4)
 *
 * Vector I/O.  musl's buffered stdio drives file output through writev
 * (__stdio_write gathers the unflushed prefix + the new bytes into two
 * iovec entries) and input through readv, so a static musl printf needs
 * writev -- not write -- to actually emit bytes.  Each handler walks the
 * user iovec array and delegates one sys_write / sys_read per segment.
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// writev(fd, iov, iovcnt) -- write iov[0..iovcnt) to fd; returns total bytes.
int64_t sys_writev(uint64_t fd, uint64_t iov_virt, uint64_t iovcnt, uint64_t, uint64_t, uint64_t);

/// readv(fd, iov, iovcnt) -- read from fd into iov[0..iovcnt); returns total bytes.
int64_t sys_readv(uint64_t fd, uint64_t iov_virt, uint64_t iovcnt, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
