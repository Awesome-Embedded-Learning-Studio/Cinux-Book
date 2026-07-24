/**
 * @file kernel/syscall/sys_uname.hpp
 * @brief sys_uname handler declaration (F-ECO busybox sh smoke)
 *
 * Fills a Linux utsname so busybox `uname` / `sh` (prompt hostname) work.
 * Static "Cinux" identity -- a real nodename (hostname syscall / /etc/hostname)
 * is a follow-up.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Linux struct utsname (x86-64): six 65-byte fields = 390 bytes.
struct kutsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
static_assert(sizeof(kutsname) == 390, "utsname is 6*65 on x86-64");

/// uname(utsname) -- fill the identity; returns 0.
int64_t sys_uname(uint64_t ts_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// Tests call this; sys_uname is the user boundary (copy_to_user).
int64_t do_uname_kernel(kutsname* out);

}  // namespace cinux::syscall
