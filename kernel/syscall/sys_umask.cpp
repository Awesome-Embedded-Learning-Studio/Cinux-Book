/**
 * @file kernel/syscall/sys_umask.cpp
 * @brief sys_umask handler (F-ECO batch 2)
 *
 * Sets the per-task file-creation mask (Task::umask), returning the previous
 * one. Pure process state -- does not touch the filesystem.
 */

#include "kernel/syscall/sys_umask.hpp"

#include <stdint.h>

#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_umask(uint64_t mask, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return 0;  // No current task (boot context): report the default mask.
    }
    uint32_t old      = task->umask;
    task->umask       = static_cast<uint32_t>(mask) & 0777;
    return static_cast<int64_t>(old);
}

}  // namespace cinux::syscall
