/**
 * @file kernel/syscall/sys_brk.cpp
 * @brief sys_brk handler implementation (F2-M3)
 *
 * Moves the program break within [brk_initial, brk_max].  Lazy: no pages are
 * mapped here -- the Heap VMA created by execve covers the window and the
 * demand-paging path services the first access to a heap page.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_brk.hpp"

#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return 0;
    }

    // Query: addr == 0 returns the current break unchanged.
    if (addr == 0) {
        return static_cast<int64_t>(task->brk_current);
    }

    // Honour the request only within [brk_initial, brk_max]; otherwise return
    // the current break unchanged (Linux semantics -- brk does not fail).
    if (addr < task->brk_initial || addr > task->brk_max) {
        return static_cast<int64_t>(task->brk_current);
    }

    task->brk_current = addr;
    return static_cast<int64_t>(addr);
}

}  // namespace cinux::syscall
