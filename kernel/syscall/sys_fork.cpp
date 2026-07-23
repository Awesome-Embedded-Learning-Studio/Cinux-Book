/**
 * @file kernel/syscall/sys_fork.cpp
 * @brief sys_fork handler implementation
 *
 * Delegates to cinux::proc::fork() which performs the actual
 * Copy-On-Write fork of the current task.
 */

#include "kernel/syscall/sys_fork.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"

namespace cinux::syscall {

int64_t sys_fork(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
    return static_cast<int64_t>(child_pid);
}

// B3b: vfork -- busybox init's respawn primitive.  See sys_fork.hpp: parent
// immediately waitpid()s the child, so a CoW fork is safe (no shared-stack
// window the caller relies on).
int64_t sys_vfork(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    return sys_fork(a, b, c, d, e, f);
}

}  // namespace cinux::syscall
