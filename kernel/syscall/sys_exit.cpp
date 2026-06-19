/**
 * @file kernel/syscall/sys_exit.cpp
 * @brief sys_exit handler implementation
 *
 * Marks the current task as Dead and yields to the scheduler.
 */

#include "kernel/syscall/sys_exit.hpp"

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/syscall/sys_futex.hpp"

namespace cinux::proc {

void task_exit_cleartid(Task* task) {
    if (task == nullptr || task->clear_child_tid == 0) {
        return;
    }
    // CLONE_CHILD_CLEARTID: zero the child_tid word and wake one futex waiter
    // (the pthread_join protocol).  The address is in the caller's address
    // space, which the kernel maps, so a direct write is safe.
    *reinterpret_cast<volatile uint32_t*>(task->clear_child_tid) = 0;
    cinux::syscall::futex_wake_addr(task->clear_child_tid, 1);
}

}  // namespace cinux::proc

namespace cinux::syscall {

int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task != nullptr) {
        // F3-M2: CLONE_CHILD_CLEARTID -- zero child_tid + wake joiner before
        // the task goes away.
        cinux::proc::task_exit_cleartid(task);
        // F3-M1: notify the parent of our exit.  SIGCHLD's default disposition
        // is Ignore, but a parent with a handler or one polling waitpid uses it.
        if (task->parent != nullptr) {
            cinux::proc::signal_send(task->parent, cinux::proc::Signal::kSigchld);
        }
        task->state = cinux::proc::TaskState::Dead;
        cinux::lib::kprintf("[SYSCALL] sys_exit(%u) from tid=%u '%s'\n",
                            static_cast<unsigned>(code), static_cast<unsigned>(task->tid),
                            task->name);
    }

    if (cinux::proc::Scheduler::is_initialized()) {
        cinux::proc::Scheduler::yield();
    } else {
        cinux::lib::kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }

    return 0;
}

}  // namespace cinux::syscall
