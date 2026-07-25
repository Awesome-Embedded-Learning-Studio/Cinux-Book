/**
 * @file kernel/syscall/sys_getcwd.hpp
 * @brief sys_getcwd handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Get the current working directory
 *
 * Copies the current task's cwd string into the user buffer.
 *
 * @param buf_virt  User virtual address of the output buffer
 * @param size      Size of the output buffer in bytes
 * @return Number of bytes written (including NUL), or -1 on error
 */
int64_t sys_getcwd(uint64_t buf_virt, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t);

/// P0e (SMAP): write cwd into a KERNEL buffer (no user memory). Tests call this.
int64_t do_getcwd_kernel(char* dst, uint32_t size);

}  // namespace cinux::syscall
