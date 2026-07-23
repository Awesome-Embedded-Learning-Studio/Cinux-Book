/**
 * @file kernel/syscall/sys_pipe.hpp
 * @brief sys_pipe handler declaration
 *
 * Creates an anonymous unidirectional pipe and installs two file
 * descriptors in the calling process's FDTable:
 *   - pipefd[0] is the read end
 *   - pipefd[1] is the write end
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::fs {
class FDTable;
}

namespace cinux::syscall {

int64_t sys_pipe(uint64_t pipefd_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// P0e (SMAP): build the pipe graph + allocate two fds into a KERNEL int[2].
/// Tests call this; sys_pipe is the user boundary (copy_to_user the int[2]).
int64_t do_pipe_kernel(cinux::fs::FDTable& tbl, int* pipefd_kernel);

}  // namespace cinux::syscall
