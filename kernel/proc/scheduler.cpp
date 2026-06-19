#include "kernel/proc/scheduler.hpp"

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/per_cpu.hpp"

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

    task->state        = TaskState::Running;
    // A freshly scheduled task starts with a full time quantum.
    quantum_remaining_ = Scheduler::DEFAULT_TIME_SLICE;

    // Re-enqueue at the tail so the task keeps cycling (round-robin within its
    // own priority level) rather than being dropped after one run.
    run_queue_[tail_] = task;
    tail_             = (tail_ + 1) % MAX_TASKS;
    count_++;

    return task;
}

const char* RoundRobin::name() const {
    return "RoundRobin";
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

// ============================================================
// PerCPU global
// ============================================================

PerCPU g_per_cpu{nullptr, 0, 0};

// ============================================================
// Address space switch (Linux switch_mm style)
// ============================================================

namespace {

void switch_addr_space(Task* prev, Task* next) {
    if (prev->addr_space == next->addr_space) {
        return;
    }
    if (next->addr_space) {
        next->addr_space->activate();
    } else {
        cinux::arch::write_cr3(cinux::mm::AddressSpace::kernel_pml4());
    }
}

}  // namespace

// ============================================================
// Scheduler static state
// ============================================================

SchedulingClass* Scheduler::classes_[Scheduler::MAX_CLASSES];
int              Scheduler::class_count_ = 0;
Task*            Scheduler::current_     = nullptr;
RoundRobin       Scheduler::default_rr_;
Task*            Scheduler::idle_task_   = nullptr;
bool             Scheduler::initialized_ = false;
lib::Atomic<int> Scheduler::tick_count_{0};

// ============================================================
// Scheduler implementation
// ============================================================

void Scheduler::idle_entry() {
    while (true) {
        __asm__ volatile("hlt");
    }
}

void Scheduler::init() {
    class_count_ = 0;
    current_     = nullptr;
    idle_task_   = nullptr;
    tick_count_.store(0, lib::MemoryOrder::Relaxed);
    register_class(&default_rr_);

    idle_task_ = TaskBuilder().set_entry(idle_entry).set_name("idle").set_priority(255).build();

    if (idle_task_ != nullptr) {
        idle_task_->state = TaskState::Ready;
        cinux::lib::kprintf("[SCHED] Idle task created tid=%u\n", idle_task_->tid);
    }

    initialized_ = true;
    cinux::lib::kprintf("[SCHED] Scheduler initialised with %s class\n", default_rr_.name());
}

void Scheduler::register_class(SchedulingClass* sched_class) {
    if (class_count_ >= MAX_CLASSES) {
        cinux::lib::kprintf("[SCHED] Too many scheduling classes\n");
        return;
    }
    classes_[class_count_++] = sched_class;
}

Task* Scheduler::pick_next_from(SchedulingClass** classes, int count) {
    // Precedence is array order: the first class with a runnable task wins,
    // later classes are only consulted once every earlier class is empty.
    for (int i = 0; i < count; i++) {
        if (Task* next = classes[i]->pick_next()) {
            return next;
        }
    }
    return nullptr;
}

Task* Scheduler::pick_next_task() {
    return pick_next_from(classes_, class_count_);
}

void Scheduler::add_task(Task* task) {
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);
    signal_register_task(task);
    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' added to %s\n", task->tid, task->name,
                        task->sched_class->name());
}

void Scheduler::remove_task(Task* task) {
    if (task == nullptr) {
        return;
    }
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }
    signal_unregister_task(task);
    task->state = TaskState::Dead;
    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' removed\n", task->tid, task->name);
}

void Scheduler::yield() {
    if (current_ == nullptr) {
        return;
    }

    schedule();
}

void Scheduler::exit_current() {
    Task* prev = current_;
    if (prev != nullptr) {
        prev->state = TaskState::Dead;
        prev->sched_class->dequeue(prev);
        signal_unregister_task(prev);
        cinux::lib::kprintf("[SCHED] Task tid=%u '%s' exited\n", prev->tid, prev->name);
    }

    Task* next = pick_next_task();
    if (next == nullptr) {
        if (idle_task_ != nullptr) {
            next = idle_task_;
        } else {
            cinux::lib::kprintf("[SCHED] No more tasks, halting.\n");
            while (1)
                __asm__ volatile("cli; hlt");
        }
    }

    current_          = next;
    g_per_cpu.current = next;
    if (next != idle_task_) {
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
        g_per_cpu.update_syscall_stack(next->kernel_stack_top);
    }
    __asm__ volatile("fxsave %0" : : "m"(prev->fpu_state));
    switch_addr_space(prev, next);
    context_switch(&prev->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current_->fpu_state));
}

void Scheduler::run_first(Task* boot_task) {
    current_          = boot_task;
    g_per_cpu.current = boot_task;
    cinux::arch::GDT::tss_set_rsp0(boot_task->kernel_stack_top);

    Task* next = pick_next_task();
    if (next == nullptr) {
        return;
    }

    current_          = next;
    g_per_cpu.current = next;
    cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
    g_per_cpu.update_syscall_stack(next->kernel_stack_top);
    __asm__ volatile("fxsave %0" : : "m"(boot_task->fpu_state));
    switch_addr_space(boot_task, next);
    context_switch(&boot_task->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current_->fpu_state));
}

Task* Scheduler::current() {
    return current_;
}

void Scheduler::set_current(Task* task) {
    current_          = task;
    g_per_cpu.current = task;
}

bool Scheduler::is_initialized() {
    return initialized_;
}

void Scheduler::tick() {
    if (!initialized_ || current_ == nullptr) {
        return;
    }

    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);

    // Preemption policy is owned by the task's scheduling class.  The class
    // returns true when the running task should yield its time slice.
    if (current_->sched_class != nullptr && current_->sched_class->task_tick(current_)) {
        schedule();
    }
}

void Scheduler::schedule() {
    if (current_ == nullptr) {
        return;
    }

    Task* prev = current_;

    if (prev->state == TaskState::Running) {
        prev->state = TaskState::Ready;
    }

    Task* next = pick_next_task();

    if (next == nullptr || next == prev) {
        // F3-M3 batch 4a: a Zombie task (exited, awaiting reap) must never be
        // rescheduled -- pick_next() is state-blind, so guard here as well.
        // F3-M4 batch 4: a Stopped task (job-control) likewise must not keep
        // running.
        if (prev->state != TaskState::Blocked && prev->state != TaskState::Dead &&
            prev->state != TaskState::Zombie && prev->state != TaskState::Stopped) {
            prev->state = TaskState::Running;
            return;
        }

        if (idle_task_ != nullptr && idle_task_ != prev) {
            next = idle_task_;
        } else {
            return;
        }
    }

    current_          = next;
    g_per_cpu.current = next;

    if (next != idle_task_) {
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
        g_per_cpu.update_syscall_stack(next->kernel_stack_top);
    }

    __asm__ volatile("fxsave %0" : : "m"(prev->fpu_state));
    switch_addr_space(prev, next);
    context_switch(&prev->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current_->fpu_state));
}

void Scheduler::block(Task* task, const char* reason) {
    if (task == nullptr) {
        return;
    }

    task->state = TaskState::Blocked;
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }

    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' blocked: %s\n", task->tid, task->name,
                        reason ? reason : "unknown");

    if (task == current_) {
        schedule();
    }
}

void Scheduler::unblock(Task* task) {
    if (task == nullptr) {
        return;
    }

    task->state = TaskState::Ready;
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);

    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' unblocked\n", task->tid, task->name);
}

}  // namespace cinux::proc
