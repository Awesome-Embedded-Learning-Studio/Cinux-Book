#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/lib/atomic.hpp"
#include "kernel/lib/not_null.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::proc {

// ============================================================
// Pluggable scheduling (F3-M4)
// ============================================================
//
// The scheduler is policy-pluggable: a scheduling algorithm is just a
// SchedulingClass subclass.  Adding one (e.g. a strict-priority class) is:
//
//   class PriorityScheduler : public SchedulingClass {
//    public:
//     void  enqueue(Task* t) override      { /* insert ordered by priority */ }
//     void  dequeue(Task* t) override      { /* remove t from the queue */ }
//     Task* pick_next() override           { /* return highest-priority task */ }
//     const char* name() const override    { return "Priority"; }
//     // Optional policy hooks (defaults are no-ops):
//     bool task_tick(Task* cur) override   { /* true => request preemption */ }
//     void task_fork(Task* p, Task* c) override { /* derive child params */ }
//    private:
//     /* run-queue state + a Spinlock */
//   };
//
//   // At boot, after Scheduler::init() registers the default RoundRobin:
//   Scheduler::register_class(&my_priority_class);
//
// Scheduler::pick_next_task() asks each registered class in registration order
// (index 0 = highest precedence) for a task; the first non-empty class wins.
// A task picks its class via Task::sched_class (default &default_rr_); see
// TaskBuilder::set_sched_class().  RoundRobin itself is a worked example --
// priority-aware selection (lower Task::priority runs first) plus a 2-tick
// quantum driven by task_tick().

class SchedulingClass {
public:
    virtual ~SchedulingClass() = default;

    // --- Run-queue management (required) ---
    virtual void        enqueue(Task* task) = 0;
    virtual void        dequeue(Task* task) = 0;
    virtual Task*       pick_next()         = 0;
    virtual const char* name() const        = 0;

    // --- Policy hooks (optional; defaults preserve legacy behaviour) ---

    // Called once per timer tick for the running task.  Return true to ask the
    // scheduler to preempt (e.g. the time quantum is exhausted); false lets the
    // task keep running.  Lets each class own its own preemption policy instead
    // of hard-coding it in Scheduler::tick.
    virtual bool task_tick(Task* current);

    // Called when a task is forked/cloned so the class can derive the child's
    // scheduling parameters from the parent (e.g. inherit priority).
    virtual void task_fork(Task* parent, Task* child);

    // Deadline tick for deadline-based (real-time) scheduling, or 0 when the
    // class is not deadline-based.  Reserved for future RT scheduling classes.
    virtual uint64_t task_deadline(Task* task);

    // Is the run queue empty?  Pure peek (no dequeue).  Used by the AP idle loop
    // (F4-M4 M4-2-2) to re-check for runnable work under cli before sti;hlt,
    // closing the lost-wakeup window.  Conservative default: a class that does
    // not override this reports "non-empty" so the idle loop re-runs schedule().
    virtual bool is_empty() const { return false; }
};

class RoundRobin : public SchedulingClass {
public:
    static constexpr int MAX_TASKS = 64;

    RoundRobin();

    void        enqueue(Task* task) override;
    void        dequeue(Task* task) override;
    Task*       pick_next() override;
    const char* name() const override;
    bool        is_empty() const override;  ///< peek count_==0 under lock_ (F4-M4 M4-2-2)

    // Drop every queued task and reset the ring-buffer pointers.  Used by
    // Scheduler::init() so each (re)init starts from a pristine run queue --
    // the in-kernel test suite calls init() before every test, and without this
    // the tasks a previous test added would leak into the next one.  A no-op
    // at boot, where the queue is already empty.
    void clear();

    bool task_tick(Task* current) override;
    void task_fork(Task* parent, Task* child) override;
    // task_deadline is inherited unchanged (0 = not deadline-based).

private:
    // Remove the slot at logical index i (0-based from head_) and compact the
    // ring buffer around it.  Caller must hold lock_.
    void remove_at_locked(int i);

    Task*            run_queue_[MAX_TASKS];
    int              head_;
    int              tail_;
    int              count_;
    mutable Spinlock lock_;  ///< mutable: is_empty() peeks count_ under lock (F4-M4 M4-2-2)
};

class Scheduler {
public:
    static constexpr int MAX_CLASSES        = 4;
    static constexpr int DEFAULT_TIME_SLICE = 2;

    static void init();
    static void register_class(SchedulingClass* sched_class);
    static void add_task(lib::NotNull<Task*> task);
    static void remove_task(lib::NotNull<Task*> task);
    static void yield();
    static void exit_current();

    /// Reap tasks whose exit_current() deferred their free (Q4e-3 / DEBT-002).
    /// Called at schedule() entry by whichever task runs next.
    static void reap_deferred();
    static void run_first(lib::NotNull<Task*> boot_task);

    // --- Per-CPU idle tasks (F4-M4 M4-2-2) ---
    //
    // Each CPU has its own idle task (idle_tasks_[cpu_id]).  Sharing a single
    // idle task across CPUs is fatal: two CPUs context-switching through it
    // would share one ctx and one 16 KB stack.  The BSP's idle (idle_tasks_[0])
    // keeps the legacy idle_entry() (while-hlt, driven by PIT ticks); each AP
    // builds its own via setup_ap_idle() with ap_idle_entry().  M4-2-2 ships
    // ap_idle_entry() as a pure sti;hlt (woken by the reschedule IPI) -- APs do
    // NOT yet run user tasks, because migrating one to an AP GP-faults (M4-3).
    // The schedule()+sti;hlt runq-pulling loop re-enables there once migration
    // is safe; has_runnable_task()/is_empty() already back its lost-wakeup check.

    /// This CPU's idle task.  BSP returns idle_tasks_[0].
    static Task* idle();

    /// Build idle_tasks_[cpu_id] (AP idle, entry=ap_idle_entry) if absent and
    /// return it.  Idempotent: no-op for cpu 0 (BSP idle is built in init()) and
    /// if already built.  Called by each AP after Scheduler::init() has run.
    static Task* setup_ap_idle(uint32_t cpu_id);

    static Task* current();                // reads this CPU's PerCpu::current (per-CPU since M4-1)
    static void  set_current(Task* task);  // nullable: tests clear per-CPU current with nullptr
    static bool  is_initialized();

    // In-kernel test-harness role-play guard.  While at least one
    // NoRescheduleGuard is alive, block() still transitions the task to Blocked
    // and removes it from the run queue, but does NOT context-switch away.  The
    // test harness runs single-threaded with no real dispatch loop, role-playing
    // tasks by installing them as current via set_current(); suppressing
    // block()'s reschedule lets the harness thread keep observing wait-queue /
    // task state -- exactly as a second CPU watching a blocked task would.
    // Production never raises the depth (it stays 0), so this is inert in the
    // real kernel.  Only block()'s and schedule_blocked()'s schedule() are gated;
    // tick()/yield()/exit_current() are untouched.
    class NoRescheduleGuard {
    public:
        NoRescheduleGuard();
        ~NoRescheduleGuard();
    };

    static void tick();
    static void schedule();
    static void block(lib::NotNull<Task*> task, const char* reason);
    static void unblock(lib::NotNull<Task*> task);

    // --- Prepare-to-wait (F4-M4): lost-wakeup-safe blocking for wait paths ---
    //
    // The classic wait pattern -- "drop the waiter-lock, THEN block()" -- has a
    // lost-wakeup window once a second CPU can run the waker concurrently:
    // between lock release and block(), a concurrent unblock() can flip the task
    // Ready and enqueue it, only for this CPU's block() to flip it back Blocked
    // and dequeue it -- the wakeup is lost and the task sleeps forever.
    //
    // prepare_to_wait() + schedule_blocked() close the window the way Linux's
    // prepare_to_wait()/schedule() do:
    //   1. Under the waiter-lock (and irq_guard, so no local tick preempts us):
    //        enqueue onto the wait queue;
    //        prepare_to_wait(self);   // self->state = Blocked  (atomic vs waker)
    //   2. Release the waiter-lock (re-enable IRQs).
    //   3. schedule_blocked();        // actually switch out
    // If a concurrent unblock() raced through the window, self is already Ready
    // and schedule()'s next==prev path keeps it running instead of sleeping --
    // no lost wakeup.  schedule_blocked() is the NoRescheduleGuard-aware partner
    // (the in-kernel test harness role-plays waits without a dispatch loop).

    /// Mark @p task about to sleep (state -> Blocked) WITHOUT switching away.
    /// Caller holds the waiter-lock under irq_guard; follow with schedule_blocked().
    static void prepare_to_wait(lib::NotNull<Task*> task);

    /// Switch away from the current task unless a NoRescheduleGuard is active.
    /// The wait-path partner of prepare_to_wait(); production (depth == 0) always
    /// switches.  Equivalent to block()'s tail for the prepare-to-wait pattern.
    static void schedule_blocked();

    // Ask each class in `classes` (precedence = array order, index 0 first) for
    // its next runnable task; return the first non-null, or nullptr if every
    // class is empty.  Exposed as a primitive so the multi-class traversal is
    // unit-testable in isolation, independent of the global class table.
    static Task* pick_next_from(SchedulingClass** classes, int count);

private:
    static void  idle_entry();      // BSP idle: while-hlt (PIT-tick driven)
    static void  ap_idle_entry();   // AP idle: schedule()+sti;hlt loop (IPI driven), F4-M4 M4-2-2
    static Task* pick_next_task();  // pick_next_from(classes_, class_count_)

    /// Best-effort peek: any runnable task on the shared run queue?  Pure state
    /// check (no dequeue).  Used by ap_idle_entry's lost-wakeup recheck under cli.
    static bool has_runnable_task();

    static SchedulingClass* classes_[MAX_CLASSES];
    static int              class_count_;
    static RoundRobin       default_rr_;
    static Task*            idle_tasks_[kMaxCpus];  ///< per-CPU idle; [0]=BSP (F4-M4 M4-2-2)
    static bool             initialized_;
    static lib::Atomic<int> tick_count_;
    static int              no_reschedule_depth_;  ///< >0 only inside the test harness
};

}  // namespace cinux::proc
