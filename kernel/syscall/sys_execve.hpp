/**
 * @file kernel/syscall/sys_execve.hpp
 * @brief sys_execve handler declaration
 *
 * Replaces the current process image with a new ELF executable
 * loaded from the VFS.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Execute a new program in the current process
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @param argv_virt  User virtual address of the argument vector
 * @param envp_virt  User virtual address of the environment vector
 * @return 0 on success, or a negative error code on failure
 */
int64_t sys_execve(uint64_t path_virt, uint64_t argv_virt, uint64_t envp_virt, uint64_t, uint64_t,
                   uint64_t);

/// P0c (SMAP): pure kernel-to-kernel execve (KERNEL path/argv/envp -> ELF load
/// + initial stack + jump to user). Kernel-internal callers and tests use this;
/// sys_execve is the user boundary that stages the strings via accessors.
/// Does not return on success.
int64_t do_execve_kernel(const char* kpath, const char* const* kargv, const char* const* kenvp);

}  // namespace cinux::syscall
