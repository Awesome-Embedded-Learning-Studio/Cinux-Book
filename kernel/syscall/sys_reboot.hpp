/**
 * @file kernel/syscall/sys_reboot.hpp
 * @brief sys_reboot handler (B3b busybox-init)
 *
 * Linux reboot(2) powers off / restarts / halts the system.  Cinux does not
 * implement reboot -- it is a hobby OS that exits via isa-debug-exit, not
 * reboot.  busybox init probes reboot during startup; returning -EPERM (not
 * permitted even for root) lets init continue without depending on it.
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// reboot(2) -- not supported; returns -EPERM (B3b).
/// @p magic1 / @p magic2 are the LINUX_REBOOT_MAGIC values (ignored).
/// @p cmd is the LINUX_REBOOT_CMD_* action (ignored).
int64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
