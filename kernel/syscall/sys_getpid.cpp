/**
 * @file kernel/syscall/sys_getpid.cpp
 * @brief sys_getpid handler implementation
 *
 * Returns the thread-group ID (tgid) of the current task.  For a
 * single-threaded process tgid == pid; for threads of one process all share
 * the leader's tgid, so getpid() reports the same value in every thread
 * (F3-M2 batch 4).
 */

#include "kernel/syscall/sys_getpid.hpp"

#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        return -1;
    }
    return static_cast<int64_t>(task->tgid);
}

}  // namespace cinux::syscall
