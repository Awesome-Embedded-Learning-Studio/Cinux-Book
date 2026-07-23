/**
 * @file kernel/syscall/sys_dup.hpp
 * @brief sys_dup / sys_dup2 handler declarations (F-ECO batch 4)
 *
 * Duplicate a file descriptor.  dup() picks the lowest free fd; dup2() forces a
 * specific target fd (closing it first if open).  Both delegate to
 * FDTable::dup/dup2, which create a NEW File copying the source inode + offset +
 * flags + cloexec (a hobby-OS independent description, NOT Linux's shared
 * description -- see file.hpp).  The "质变点" for sh + pipe + redirect: a dup2'd
 * fd reaches the same inode, so `dup2(pipe_wfd, STDOUT)` redirects output.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// dup(fd) -- duplicate to the lowest free fd.  Returns the new fd, or -EBADF /
/// -EMFILE.
int64_t sys_dup(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// dup2(oldfd, newfd) -- duplicate @p oldfd onto @p newfd (closing @p newfd if
/// open).  Returns @p newfd, or -EBADF.
int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
