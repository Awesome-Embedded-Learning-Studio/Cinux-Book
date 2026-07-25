/**
 * @file kernel/syscall/sys_reboot.cpp
 * @brief sys_reboot handler (B3b busybox-init)
 */

#include "kernel/syscall/sys_reboot.hpp"

#include "kernel/errno.hpp"

namespace cinux::syscall {

int64_t sys_reboot(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Cinux does not implement reboot (exits via isa-debug-exit, not reboot).
    // -EPERM lets busybox init's startup probe fail gracefully.
    return -cinux::kEperm;
}

}  // namespace cinux::syscall
