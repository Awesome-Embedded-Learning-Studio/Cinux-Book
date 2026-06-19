/**
 * @file kernel/syscall/sys_pgrp.cpp
 * @brief Process-group / session syscall handlers (F3-M3 batch 3)
 *
 * Translation layer: resolve pid==0 to the caller, delegate to the
 * cinux::proc identity helpers, and pass the proc result (>= 0 / -errno)
 * straight back as the syscall return value.
 */

#include "kernel/syscall/sys_pgrp.hpp"

#include "kernel/proc/process.hpp"
#include "kernel/proc/process_group.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"

namespace cinux::syscall {

using cinux::proc::Task;
using cinux::proc::Scheduler;

// POSIX: a pid of 0 (or the caller's own pid) targets the calling task;
// any other pid is resolved through the signal pid registry.
static Task* resolve_target(int pid) {
    Task* cur = Scheduler::current();
    if (pid == 0 || (cur != nullptr && pid == cur->pid)) {
        return cur;
    }
    return cinux::proc::signal_find_task_by_pid(pid);
}

int64_t sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg, uint64_t, uint64_t, uint64_t, uint64_t) {
    Task* target = resolve_target(static_cast<int>(pid_arg));
    if (target == nullptr) {
        return -3;  // ESRCH
    }
    return cinux::proc::setpgid(target, static_cast<int>(pgid_arg));
}

int64_t sys_getpgid(uint64_t pid_arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    Task* target = resolve_target(static_cast<int>(pid_arg));
    if (target == nullptr) {
        return -3;  // ESRCH
    }
    return cinux::proc::getpgid(target);
}

int64_t sys_getsid(uint64_t pid_arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    Task* target = resolve_target(static_cast<int>(pid_arg));
    if (target == nullptr) {
        return -3;  // ESRCH
    }
    return cinux::proc::getsid(target);
}

int64_t sys_setsid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return cinux::proc::setsid(Scheduler::current());
}

}  // namespace cinux::syscall
