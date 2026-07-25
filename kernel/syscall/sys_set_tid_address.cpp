/**
 * @file kernel/syscall/sys_set_tid_address.cpp
 * @brief sys_set_tid_address handler implementation (F10-M1 batch 4)
 *
 * Mirrors Linux: store @p tidptr into clear_child_tid (the same field
 * CLONE_CHILD_CLEARTID sets) so task_exit_cleartid() zeroes it and wakes
 * any futex waiter on exit, then return the caller's tid.
 */

#include "kernel/syscall/sys_set_tid_address.hpp"

#include <stdint.h>

#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_set_tid_address(uint64_t tidptr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return 0;
    }
    task->clear_child_tid = tidptr;
    return static_cast<int64_t>(task->tid);
}

}  // namespace cinux::syscall
