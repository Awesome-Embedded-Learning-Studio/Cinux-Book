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
        // Record the exit code for a reaping waitpid().  Without this the
        // fork-initialized exit_status==0 leaks regardless of the real code.
        task->exit_status = static_cast<int>(code);
        // F3-M2: CLONE_CHILD_CLEARTID -- zero child_tid + wake joiner before
        // the task goes away.
        cinux::proc::task_exit_cleartid(task);
        // F3-M1: notify the parent of our exit.  SIGCHLD's default disposition
        // is Ignore, but a parent with a handler or one polling waitpid uses it.
        if (task->parent != nullptr) {
            cinux::proc::signal_send(task->parent, cinux::proc::Signal::kSigchld);
        }
        // F3-M3 batch 4a: become a Zombie (not Dead) so a wait()ing parent can
        // reap us via the children list.  Dequeue from the run queue (mirroring
        // exit_current) -- RoundRobin::pick_next() does not check state, so a
        // task left in the queue would be picked, force-set Running, and run
        // dead.  waitpid() later flips a reaped Zombie to Dead.
        task->state = cinux::proc::TaskState::Zombie;
        if (task->sched_class != nullptr) {
            task->sched_class->dequeue(task);
        }
        // F3-M3 batch 4b: wake a parent blocked in waitpid() so it re-scans
        // and reaps us (we are now Zombie).  F-QA Q4c-1 (DEBT-004): unblock
        // unconditionally -- unblock() is idempotent (no-op unless the parent
        // is actually Blocked), so this is safe even if the parent is not
        // waiting.  The old waiting_for_child gate was a plain bool read
        // cross-CPU with no barrier -> a stale read skipped the wake and
        // leaked the parent into a permanent sleep (a top flaky-hang suspect).
        if (task->parent != nullptr) {
            cinux::proc::Scheduler::unblock(task->parent);
        }
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

int64_t sys_exit_group(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // F10-M1 batch 4: musl exit() calls exit_group.  Single-threaded today,
    // so terminating the current task matches exit(); when thread groups are
    // exercised this should walk the group and reap each thread first.
    return sys_exit(code, 0, 0, 0, 0, 0);
}

}  // namespace cinux::syscall
