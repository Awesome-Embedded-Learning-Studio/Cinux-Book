/**
 * @file kernel/syscall/sys_fcntl.hpp
 * @brief sys_fcntl handler declaration (F-ECO batch 4)
 *
 * Supported cmd subset (Linux x86_64 values):
 *   F_DUPFD (0)  -- duplicate fd to the lowest free fd >= arg (like dup w/ min)
 *   F_GETFD (1)  -- read the fd flags (FD_CLOEXEC)
 *   F_SETFD (2)  -- set the fd flags (FD_CLOEXEC)
 *   F_GETFL (3)  -- read the open status flags (access mode: O_RDONLY/WRITE/RDWR)
 *
 * Deferred (return -EINVAL): F_SETFL (4) -- runtime O_NONBLOCK would need a
 * File status-flags field + plumbing into pipe/socket ops (today nonblock is
 * baked at open via FIFO open() flags).  Unknown cmds also -EINVAL.
 *
 * FD_CLOEXEC is stored per-File (File::cloexec).  execve does not yet consult
 * it (close-on-exec enforcement is a follow-up); fcntl round-trips correctly.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// fcntl(fd, cmd, arg) -- manipulate a file descriptor.  Returns >=0 (a value
/// or new fd for F_DUPFD) on success, or -errno.
int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
