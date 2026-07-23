/**
 * @file kernel/proc/scheduler_block.cpp
 * @brief Task block / unblock / lost-wakeup-safe wait machinery
 *
 * Split from scheduler.cpp to keep that file under the 500-line cap. Holds the
 * blocking primitives: direct block()/unblock() (management + test-driven) and
 * the prepare_to_wait() + schedule_blocked() pair that wait paths use to close
 * the cross-CPU lost-wakeup window. All are Scheduler static methods; the core
 * schedule()/exit_current()/idle paths stay in scheduler.cpp.
 */

#include "kernel/arch/x86_64/smp.hpp"  // arch::wake_idle_ap (unblock)
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

// Direct, unconditional block of @p task: mark it Blocked, drop it from the run
// queue, and (if it is the running task) switch away.  Used for management /
// test-driven blocks (test_block_unblock, test_block_dispatches).  Wait paths
// that must be lost-wakeup-safe across CPUs use prepare_to_wait() +
// schedule_blocked() instead (see scheduler.hpp).  (F4-M4: role clarified.)
void Scheduler::block(lib::NotNull<Task*> task, const char* reason) {
    if (task == nullptr) {
        return;
    }

    task->state = TaskState::Blocked;
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }

    cinux::lib::kprintf("[SCHED] Task tid=%lu '%s' blocked: %s\n", task->tid, task->name,
                        reason ? reason : "unknown");

    // Context-switch away only when the blocked task is the running one AND a
    // real dispatch loop is active.  Inside a NoRescheduleGuard (in-kernel test
    // harness role-play) we skip the switch so the harness thread can observe
    // the task's state, as a second CPU would.
    if (task == current() && no_reschedule_depth_ == 0) {
        schedule();
    }
}

void Scheduler::unblock(lib::NotNull<Task*> task) {
    if (task == nullptr) {
        return;
    }

    // Idempotent (F4-M4 prepare-to-wait): only a still-Blocked task needs waking.
    // A task that is already Ready/Running was never put to sleep, or already won
    // a concurrent lost-wakeup race and is runnable -- re-enqueuing it would
    // double-add it to the run queue.  No-op in that case.
    if (task->state != TaskState::Blocked) {
        return;
    }

    task->state = TaskState::Ready;
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);

    cinux::lib::kprintf("[SCHED] Task tid=%lu '%s' unblocked\n", task->tid, task->name);
    // Wake an idle AP so it can pick up this freshly runnable task (F4-M4 M4-2).
    // No-op on a single-core system.
    arch::wake_idle_ap();
}

void Scheduler::prepare_to_wait(lib::NotNull<Task*> task) {
    if (task == nullptr) {
        return;
    }
    // Flip state to Blocked under the caller's waiter-lock so a concurrent waker
    // on another CPU observes "sleeping" before it can miss the task.  current()
    // is never on the run queue, so no dequeue is needed here; the caller follows
    // with schedule_blocked().  See the prepare-to-wait contract in scheduler.hpp.
    task->state = TaskState::Blocked;
}

void Scheduler::schedule_blocked() {
    // Wait-path partner of prepare_to_wait(): switch out unless the in-kernel
    // test harness is role-playing (NoRescheduleGuard).  Production (depth == 0)
    // always switches.
    if (no_reschedule_depth_ == 0) {
        schedule();
    }
}

}  // namespace cinux::proc
