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
#include "kernel/proc/percpu.hpp"  // F4-followup: percpu()->cpu_id in pick_next()
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

RoundRobin::RoundRobin() : head_(0), tail_(0), count_(0) {
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
    //
    // F4-followup (SMP migration race): skip tasks whose on_cpu != -1 -- their
    // ctx is still being saved by the context_switch on the CPU they just left,
    // so loading their ctx now would race that save and corrupt it.  They stay
    // queued and become eligible once context_switch.S clears on_cpu to -1.
    // best == -1 means no eligible task (all mid-save): caller falls to idle.
    // F4-followup: a task with on_cpu == ANOTHER CPU is still being saved by that
    // CPU's context_switch -- loading its ctx here would race.  Skip it.  But a
    // task with on_cpu == THIS CPU is one we just yielded (its ctx save is our
    // own upcoming context_switch) -- we may pick it back (next==prev), which
    // keeps single-core round-robin correct (yield with no other runnable task
    // continues the same task instead of bouncing to idle).
    const int my_cpu = static_cast<int>(percpu()->cpu_id);
    int       best   = -1;
    for (int i = 0; i < count_; i++) {
        int   idx    = (head_ + i) % MAX_TASKS;
        Task* t      = run_queue_[idx];
        int   on_cpu = __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE);
        if (on_cpu != -1 && on_cpu != my_cpu) {
            continue;  // ctx save in flight on another CPU
        }
        if (best == -1 || t->priority < run_queue_[(head_ + best) % MAX_TASKS]->priority) {
            best = i;
        }
    }
    if (best == -1) {
        return nullptr;
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
    task->state             = TaskState::Running;
    // A freshly scheduled task starts with a full time quantum (DEBT-007: per-task).
    task->quantum_remaining = Scheduler::DEFAULT_TIME_SLICE;

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
    head_  = 0;
    tail_  = 0;
    count_ = 0;
}

bool RoundRobin::task_tick(Task* current) {
    auto g = lock_.irq_guard();
    (void)g;
    // DEBT-007: per-task quantum (was a shared RoundRobin member -> multi-core
    // tick races shrank the slice to DEFAULT_TIME_SLICE/ncpus, and one core's
    // recharge reset the other's running task).  Aligned with Linux
    // task_struct->rt.time_slice.
    if (current->quantum_remaining > 0) {
        current->quantum_remaining--;
    }
    if (current->quantum_remaining == 0) {
        // Quantum exhausted: request preemption and recharge so that, if no
        // other task is runnable, the same task is not re-preempted every tick.
        current->quantum_remaining = Scheduler::DEFAULT_TIME_SLICE;
        return true;
    }
    return false;
}

void RoundRobin::task_fork(Task* parent, Task* child) {
    // fork/clone already memcpy the whole TCB, so the child's priority is a
    // copy of the parent's today.  Centralising the rule here lets a future
    // scheduling class derive child parameters without touching fork/clone.
    if (parent != nullptr && child != nullptr) {
        child->priority          = parent->priority;
        // DEBT-007: child starts with a full quantum (memcpy copied parent's
        // remaining ticks; a fresh task should not inherit a near-expired slice).
        child->quantum_remaining = Scheduler::DEFAULT_TIME_SLICE;
    }
}

}  // namespace cinux::proc
