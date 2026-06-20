/**
 * @file kernel/proc/roundrobin.cpp
 * @brief SchedulingClass default policy hooks + RoundRobin run-queue implementation
 *
 * Split from scheduler.cpp (F4-M4 M4-2-2) to respect the 500-line file limit.
 * The default scheduling class (RoundRobin) and the SchedulingClass base hooks
 * live here; the Scheduler core (schedule/tick/block/idle/...) stays in
 * scheduler.cpp.
 */

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

// ============================================================
// SchedulingClass default policy hooks
// ============================================================

// Defaults preserve the legacy contract: no per-tick preemption, no fork
// derivation, no deadline.  A concrete class overrides only what its policy
// needs (see RoundRobin below).
bool SchedulingClass::task_tick(Task*) {
    return false;
}

void SchedulingClass::task_fork(Task*, Task*) {
    // No-op: the child keeps whatever parameters its creator gave it.
}

uint64_t SchedulingClass::task_deadline(Task*) {
    return 0;
}

// ============================================================
// RoundRobin implementation
// ============================================================

RoundRobin::RoundRobin()
    : head_(0), tail_(0), count_(0), quantum_remaining_(Scheduler::DEFAULT_TIME_SLICE) {
    for (int i = 0; i < MAX_TASKS; i++) {
        run_queue_[i] = nullptr;
    }
}

void RoundRobin::enqueue(Task* task) {
    auto g = lock_.irq_guard();
    (void)g;
    if (count_ >= MAX_TASKS) {
        cinux::lib::kprintf("[SCHED] RoundRobin: run queue full\n");
        return;
    }
    run_queue_[tail_] = task;
    tail_             = (tail_ + 1) % MAX_TASKS;
    count_++;
    task->state = TaskState::Ready;
}

void RoundRobin::remove_at_locked(int i) {
    for (int j = i; j < count_ - 1; j++) {
        int cur         = (head_ + j) % MAX_TASKS;
        int nxt         = (head_ + j + 1) % MAX_TASKS;
        run_queue_[cur] = run_queue_[nxt];
    }
    run_queue_[(head_ + count_ - 1) % MAX_TASKS] = nullptr;
    tail_                                        = (tail_ - 1 + MAX_TASKS) % MAX_TASKS;
    count_--;
}

void RoundRobin::dequeue(Task* task) {
    auto g = lock_.irq_guard();
    (void)g;
    for (int i = 0; i < count_; i++) {
        if (run_queue_[(head_ + i) % MAX_TASKS] == task) {
            remove_at_locked(i);
            return;
        }
    }
}

Task* RoundRobin::pick_next() {
    auto g = lock_.irq_guard();
    (void)g;
    if (count_ == 0) {
        return nullptr;
    }

    // Select the highest-priority ready task: priority is "lower value runs
    // first" (Linux style, matching the idle task's priority of 255).  Ties are
    // broken FIFO (earliest enqueued first) so equal-priority tasks round-robin.
    int best = 0;
    for (int i = 1; i < count_; i++) {
        int idx      = (head_ + i) % MAX_TASKS;
        int best_idx = (head_ + best) % MAX_TASKS;
        if (run_queue_[idx]->priority < run_queue_[best_idx]->priority) {
            best = i;
        }
    }
    Task* task = run_queue_[(head_ + best) % MAX_TASKS];
    remove_at_locked(best);

    // F4-M4 M4-2-2 (runqueue multi-core safety): the picked task is REMOVED from
    // the run queue (remove_at_locked above), NOT re-enqueued.  A running task
    // must not sit in the shared queue -- otherwise a second CPU's pick_next()
    // could select the same (now-Running) task and both would context-switch onto
    // one stack/ctx.  The task re-enters the queue only when it yields or blocks
    // (schedule() re-enqueues the yielding prev; unblock() re-enqueues a woken
    // task).  Single-core round-robin is preserved: schedule() re-enqueues the
    // yielding prev before picking, so a lone task picks itself and continues.
    task->state        = TaskState::Running;
    // A freshly scheduled task starts with a full time quantum.
    quantum_remaining_ = Scheduler::DEFAULT_TIME_SLICE;

    return task;
}

const char* RoundRobin::name() const {
    return "RoundRobin";
}

bool RoundRobin::is_empty() const {
    // lock_ is mutable so this const peek can take it.  count_ is the queue
    // length; ==0 means no runnable task.  Used by ap_idle_entry()'s lost-wakeup
    // recheck (has_runnable_task) under cli.
    auto g = lock_.irq_guard();
    (void)g;
    return count_ == 0;
}

void RoundRobin::clear() {
    auto g = lock_.irq_guard();
    (void)g;
    for (int i = 0; i < MAX_TASKS; i++) {
        run_queue_[i] = nullptr;
    }
    head_              = 0;
    tail_              = 0;
    count_             = 0;
    quantum_remaining_ = Scheduler::DEFAULT_TIME_SLICE;
}

bool RoundRobin::task_tick(Task* current) {
    auto g = lock_.irq_guard();
    (void)g;
    (void)current;
    if (quantum_remaining_ > 0) {
        quantum_remaining_--;
    }
    if (quantum_remaining_ == 0) {
        // Quantum exhausted: request preemption and recharge so that, if no
        // other task is runnable, the same task is not re-preempted every tick.
        quantum_remaining_ = Scheduler::DEFAULT_TIME_SLICE;
        return true;
    }
    return false;
}

void RoundRobin::task_fork(Task* parent, Task* child) {
    // fork/clone already memcpy the whole TCB, so the child's priority is a
    // copy of the parent's today.  Centralising the rule here lets a future
    // scheduling class derive child parameters without touching fork/clone.
    if (parent != nullptr && child != nullptr) {
        child->priority = parent->priority;
    }
}

}  // namespace cinux::proc
