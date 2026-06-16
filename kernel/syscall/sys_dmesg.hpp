/**
 * @file kernel/syscall/sys_dmesg.hpp
 * @brief sys_dmesg handler declaration -- read kernel log into a user buffer
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Read the kernel log history into a user buffer as text
 *
 * Drains KernelLog entries and formats each as "[LEVEL] tick: message\n"
 * into @p buf_virt, up to @p len bytes.  Mirrors the role of Linux's
 * SYS_syslog (dmesg read).
 *
 * @param buf_virt  User virtual address of the destination buffer
 * @param len       Buffer capacity in bytes
 * @return Number of bytes written (>= 0), or -errno on error
 *         (-EFAULT on an invalid buffer address)
 */
int64_t sys_dmesg(uint64_t buf_virt, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
