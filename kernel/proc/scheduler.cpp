#include "kernel/proc/scheduler.hpp"

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/lockdep.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process_internal.hpp"  // Q4e-3: free_kernel_stack
#include "kernel/proc/sync.hpp"              // Q4e-3: Spinlock (deferred list)

namespace cinux::proc {

// SchedulingClass default hooks + RoundRobin implementation moved to
// roundrobin.cpp (F4-M4 M4-2-2 split, to keep this file under the 500-line cap).

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

// F-QA Q4e-3 (DEBT-002): deferred-free list. A task exiting via exit_current()
// cannot free its own kernel stack (it runs on it); it is enqueued here and
// freed by the next task's schedule() entry (reap_deferred).
//
// PER-CPU lists (SMP correctness): exit_current() puts prev on THIS CPU's list
// and then keeps using prev (fxsave/context_switch) until the switch completes.
// With a single global list, another CPU's schedule()->reap_deferred() could
// free prev mid-switch (use-after-free -> #GP at kernel_init exit, rare race).
// Indexing by cpu_id means each CPU reaps only what it deferred -- and a CPU
// only reaps in its own schedule(), which runs AFTER that CPU's exit_current
// switch is done, so prev is never freed while still in use.
Spinlock g_deferred_lock;  // irq_guard: local IRQ safety (lists are per-CPU, uncontended cross-CPU)
Task*    g_deferred_heads[kMaxCpus] = {};

void enqueue_deferred(Task* t) {
    auto           g    = g_deferred_lock.irq_guard();
    const uint32_t c    = percpu()->cpu_id;
    t->deferred_next    = g_deferred_heads[c];
    g_deferred_heads[c] = t;
}

}  // namespace

// ============================================================
// Scheduler static state
// ============================================================

SchedulingClass* Scheduler::classes_[Scheduler::MAX_CLASSES];
int              Scheduler::class_count_ = 0;
RoundRobin       Scheduler::default_rr_;
Task*            Scheduler::idle_tasks_[kMaxCpus] = {};  // per-CPU idle; [0]=BSP (F4-M4 M4-2-2)
bool             Scheduler::initialized_          = false;
lib::Atomic<int> Scheduler::tick_count_{0};
int              Scheduler::no_reschedule_depth_ = 0;

// ============================================================
// NoRescheduleGuard (test-harness role-play)
// ============================================================

Scheduler::NoRescheduleGuard::NoRescheduleGuard() {
    no_reschedule_depth_++;
}

Scheduler::NoRescheduleGuard::~NoRescheduleGuard() {
    no_reschedule_depth_--;
}

// ============================================================
// Scheduler implementation
// ============================================================

void Scheduler::idle_entry() {
    while (true) {
        __asm__ volatile("hlt");
    }
}

Task* Scheduler::idle() {
    uint32_t cpu = percpu()->cpu_id;
    if (cpu >= kMaxCpus) {
        cpu = 0;  // defensive: GS is anchored before any schedule() runs
    }
    return idle_tasks_[cpu];
}

Task* Scheduler::setup_ap_idle(uint32_t cpu_id) {
    if (cpu_id == 0 || cpu_id >= kMaxCpus) {
        return idle_tasks_[0];  // BSP idle lives in init(); out-of-range falls back to BSP's
    }
    if (idle_tasks_[cpu_id] != nullptr) {
        return idle_tasks_[cpu_id];  // idempotent: AP re-entry guard
    }
    idle_tasks_[cpu_id] =
        TaskBuilder().set_entry(ap_idle_entry).set_name("ap_idle").set_priority(255).build();
    if (idle_tasks_[cpu_id] != nullptr) {
        idle_tasks_[cpu_id]->state = TaskState::Running;  // this AP's current()
        idle_tasks_[cpu_id]->on_cpu =
            static_cast<int>(cpu_id);  // F4-followup: AP idle runs on this AP, never via ctx save
        cinux::lib::kprintf("[SCHED] AP%u idle task created tid=%lu\n", cpu_id,
                            idle_tasks_[cpu_id]->tid);
    }
    return idle_tasks_[cpu_id];
}

bool Scheduler::has_runnable_task() {
    // Pure peek (no dequeue): used by ap_idle_entry() under cli to re-check for
    // runnable work before sti;hlt, closing the lost-wakeup window.  Each class's
    // is_empty() takes its own lock, so this is safe under cli (irq_guard restores
    // the saved IF on destruction -- never sti-s early when already cli).
    for (int i = 0; i < class_count_; i++) {
        if (!classes_[i]->is_empty()) {
            return true;
        }
    }
    return false;
}

void Scheduler::ap_idle_entry() {
    // F4-M4 M4-2-3: APs now pull user tasks off the shared run queue.  An AP
    // woken by the reschedule IPI (vector 0xE0, sent by wake_idle_ap()) re-checks
    // the run queue under cli -- has_runnable_task() is a pure peek that closes
    // the lost-wakeup window (a task enqueued between the check and sti;hlt is
    // not missed: add_task() sent the very IPI that pulled us out of hlt).  If
    // there is work, schedule() context-switches onto it; otherwise sti;hlt
    // again.
    //
    // This AP's idle task (priority 255) is the schedule() prev but never enters
    // the run queue itself (it has no sched_class -- build() does not call
    // add_task -- so schedule()'s enqueue guard skips it); it is reached only via
    // the idle fallback when the queue is empty.  The queue thus holds only real
    // tasks, and pick_next()-REMOVES the winner, so two CPUs can never double-pick
    // one.  M4-2-2's per-CPU idle tasks make each switch use this CPU's own ctx.
    while (true) {
        if (has_runnable_task()) {
            schedule();
        }
        __asm__ volatile("sti; hlt");
        __asm__ volatile("cli");
    }
}

void Scheduler::init() {
    class_count_      = 0;
    percpu()->current = nullptr;  // current() reads per-CPU (GS is anchored before init)
    for (uint32_t i = 0; i < kMaxCpus; i++) {
        idle_tasks_[i] = nullptr;  // pristine per-CPU idle table (re-init isolation)
    }
    tick_count_.store(0, lib::MemoryOrder::Relaxed);
    register_class(&default_rr_);
    default_rr_.clear();  // pristine run queue -- no leakage across re-init (tests)

    // BSP idle (cpu 0).  Keeps the legacy idle_entry() (while-hlt, driven by PIT
    // ticks).  AP idles are built lazily by setup_ap_idle() as each AP comes up.
    idle_tasks_[0] =
        TaskBuilder().set_entry(idle_entry).set_name("idle0").set_priority(255).build();
    if (idle_tasks_[0] != nullptr) {
        idle_tasks_[0]->state = TaskState::Ready;
        cinux::lib::kprintf("[SCHED] BSP idle task created tid=%lu\n", idle_tasks_[0]->tid);
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

void Scheduler::add_task(lib::NotNull<Task*> task) {
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);
    signal_register_task(task);
    cinux::lib::kprintf("[SCHED] Task tid=%lu '%s' added to %s\n", task->tid, task->name,
                        task->sched_class->name());
    // A newly runnable task may be claimable by an idle AP (F4-M4 M4-2).  No-op
    // on a single-core system.
    arch::wake_idle_ap();
}

void Scheduler::remove_task(lib::NotNull<Task*> task) {
    if (task == nullptr) {
        return;
    }
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }
    signal_unregister_task(task);
    task->state = TaskState::Dead;
    cinux::lib::kprintf("[SCHED] Task tid=%lu '%s' removed\n", task->tid, task->name);
}

// F-QA Q4e-3 (DEBT-002): free tasks that exit_current() deferred. A task on
// this list is Dead and not running on any CPU (exit_current already switched
// away from it), so freeing its stack + deleting it is safe from any caller's
// context. Snapshot under the lock, free outside it (heavy path).
void Scheduler::reap_deferred() {
    Task* head = nullptr;
    {
        auto           g    = g_deferred_lock.irq_guard();
        const uint32_t c    = percpu()->cpu_id;
        head                = g_deferred_heads[c];
        g_deferred_heads[c] = nullptr;
    }
    while (head != nullptr) {
        Task* t = head;
        head    = t->deferred_next;
        free_kernel_stack(t);
        delete t;  // -> release_resources (sig/cwd/fd + addr_space refcount)
    }
}

void Scheduler::yield() {
    if (current() == nullptr) {
        return;
    }

    schedule();
}

void Scheduler::exit_current() {
    Task* const my_idle = idle();
    Task*       prev    = current();
    if (prev != nullptr) {
        prev->state = TaskState::Dead;
        prev->sched_class->dequeue(prev);
        signal_unregister_task(prev);
        cinux::lib::kprintf("[SCHED] Task tid=%lu '%s' exited\n", prev->tid, prev->name);
        // Q4e-3 (DEBT-002): defer the free -- we run on prev's kernel stack, so
        // we cannot unmap/free it here. schedule() (next task) reaps the list.
        enqueue_deferred(prev);
    }

    Task* next = pick_next_task();
    if (next == nullptr) {
        if (my_idle != nullptr) {
            next = my_idle;
        } else {
            cinux::lib::kprintf("[SCHED] No more tasks, halting.\n");
            while (1)
                __asm__ volatile("cli; hlt");
        }
    }

    percpu()->current = next;
    // F4-followup (SMP migration race): claim next for this CPU (see schedule()).
    __atomic_store_n(&next->on_cpu, static_cast<int>(percpu()->cpu_id), __ATOMIC_RELEASE);
    if (next != my_idle) {
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
        update_syscall_stack(next->kernel_stack_top);
    }
    __asm__ volatile("fxsave %0" : : "m"(prev->fpu_state));
    switch_addr_space(prev, next);
    context_switch(&prev->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current()->fpu_state));
}

void Scheduler::run_first(lib::NotNull<Task*> boot_task) {
    percpu()->current = boot_task;
    boot_task->on_cpu = 0;  // F4-followup: runs on BSP, never went through ctx save
    cinux::arch::GDT::tss_set_rsp0(boot_task->kernel_stack_top);

    Task* next = pick_next_task();
    if (next == nullptr) {
        return;
    }

    percpu()->current = next;
    next->on_cpu      = 0;  // F4-followup: claim next for the BSP before context_switch
    cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
    update_syscall_stack(next->kernel_stack_top);
    __asm__ volatile("fxsave %0" : : "m"(boot_task->fpu_state));
    switch_addr_space(boot_task, next);
    context_switch(&boot_task->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current()->fpu_state));
}

Task* Scheduler::current() {
    return percpu()->current;
}

void Scheduler::set_current(Task* task) {
    percpu()->current = task;
}

bool Scheduler::is_initialized() {
    return initialized_;
}

void Scheduler::tick() {
    Task* cur = current();
    if (!initialized_ || cur == nullptr) {
        return;
    }

    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);

    // Preemption policy is owned by the task's scheduling class.  The class
    // returns true when the running task should yield its time slice.
    if (cur->sched_class != nullptr && cur->sched_class->task_tick(cur)) {
        schedule();
    }
}

void Scheduler::schedule() {
    if (current() == nullptr) {
        return;
    }

#ifdef CINUX_LOCKDEP
    // F-INFRA I-10 / F4-M5 R6-Part2: holding a spinlock across the context
    // switch below deadlocks single-core (the next task cannot release a lock
    // this caller still owns, and this caller never runs again). Catch it here
    // rather than as a silent hang. lockdep_held_depth() is per-CPU (Part1's
    // global counter was SMP-unsafe). kpanic is noreturn.
    if (uint32_t d = lockdep_held_depth(); d > 0) {
        cinux::lib::kpanic(
            "lockdep: schedule() called with %u spinlock(s) held -- "
            "would deadlock (held across context switch)",
            d);
    }
#endif

    Task* const my_idle = idle();  // cache this CPU's idle (avoids repeated MSR reads)
    Task*       prev    = current();

    // Q4e-3 (DEBT-002): reap tasks deferred by a prior exit_current(). Safe
    // here: those tasks are Dead (not running), and prev (current) is not on
    // the deferred list (it is still running).
    reap_deferred();

    if (prev->state == TaskState::Running) {
        prev->state = TaskState::Ready;
    }

    // F4-M4 M4-2-2 (runqueue multi-core safety): the running task is NOT in the
    // shared run queue.  A still-runnable prev (e.g. yielding) must re-enter the
    // queue so it -- or a higher-priority peer -- can be picked; pick_next() then
    // REMOVES the winner, so a running task is never double-picked across CPUs
    // (two CPUs context-switching onto one stack/ctx).  A prev that is
    // Blocked/Dead/Zombie/Stopped is not runnable and is not enqueued.
    if (prev->state == TaskState::Ready && prev->sched_class != nullptr) {
        prev->sched_class->enqueue(prev);
    }

    Task* next = pick_next_task();  // selects + removes the winner

    if (next == prev) {
        // Picked ourselves back (prev was the only runnable task).  pick_next()
        // already set prev->state = Running and removed it from the queue;
        // current() is unchanged, so keep running.  Equivalent to the legacy
        // round-robin self-pick.
        return;
    }

    if (next == nullptr) {
        // Queue empty: prev was not runnable (Blocked/Dead/Zombie/Stopped -- not
        // enqueued above).  Switch to this CPU's idle task.  (A Ready prev would
        // have been enqueued and thus picked, so we never reach here for it.)
        // F3-M3 batch 4a / F3-M4 batch 4: Zombie (exited, awaiting reap) and
        // Stopped (job-control) tasks must never keep running; they are never
        // enqueued, so they fall through to the idle switch here.
        if (prev->state != TaskState::Blocked && prev->state != TaskState::Dead &&
            prev->state != TaskState::Zombie && prev->state != TaskState::Stopped) {
            // Safety net: prev looks runnable but the queue was empty (e.g. full).
            // Keep running prev rather than deadlock.  Should not happen in practice.
            prev->state = TaskState::Running;
            return;
        }
        if (my_idle != nullptr && my_idle != prev) {
            next = my_idle;
        } else {
            return;
        }
    }

    percpu()->current = next;

    // F4-followup: claim next for this CPU before context_switch loads its ctx
    // (see Task::on_cpu).  pick_next() only returns on_cpu == -1 tasks.
    __atomic_store_n(&next->on_cpu, static_cast<int>(percpu()->cpu_id), __ATOMIC_RELEASE);

    if (next != my_idle) {
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
        update_syscall_stack(next->kernel_stack_top);
    }

    __asm__ volatile("fxsave %0" : : "m"(prev->fpu_state));
    switch_addr_space(prev, next);
    context_switch(&prev->ctx, &next->ctx);
    __asm__ volatile("fxrstor %0" : : "m"(current()->fpu_state));
}

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
