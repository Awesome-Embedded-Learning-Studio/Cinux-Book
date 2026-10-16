/**
 * @file    scheduler.hpp
 * @brief   The cooperative scheduler: book the classes, delegate the rest.
 *
 * Meyers-singleton shape after Pmm and Tick, and split the same way the
 * walk theme split TableWorld: this class is the semantics — the class
 * registry, task states, current-task bookkeeping, pick order, reclaim
 * timing — while two decisions live elsewhere. The machine reality
 * (register switching, page and memory return) goes through an
 * injected SwitchSink, and the who-runs-next policy goes through the
 * registered SchedulingClasses, consulted in registration order; a
 * task without an explicit class rides the first-registered one. The
 * kernel installs the assembly-backed sink; a host test installs a
 * recording sink and asserts the decision stream, which is the part
 * worth asserting, straight from the same object code. Time slices,
 * preemption flags and blocking grow on this seam: an interrupt that
 * only sets a flag is just one more voice asking the scheduler to
 * decide.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.6
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <concepts>

#include "cinux/container/self_list.hpp"
#include "cinux/container/static_stack.hpp"
#include "cinux/scoped_guard.hpp"
#include "cinux/singleton.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

class Scheduler;
class SchedulingClass;

/**
 * @brief         What a reaped child hands its waiting parent.
 * @since         0.2.0
 * @ingroup       kernel_proc
 */
struct ChildReceipt {
    unsigned long long pid;   ///< The child the wait retired.
    long long          code;  ///< The exit word it died carrying.
};

/**
 * @brief         What a switch executor must do for the scheduler.
 * @tparam        Sink   The executor type being checked.
 * @note          Four verbs, no more: carry a task-to-task handoff,
 *                hand control from kernel Main into a task, hand it
 *                back, and return a dead task's memory. The scheduler
 *                calls them and never cares whether registers or a
 *                log line answered.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
template <typename Sink>
concept SwitchSink = requires(Sink& sink, Task& outgoing, Task& incoming) {
    { sink.switch_tasks(outgoing, incoming) } -> std::same_as<void>;
    { sink.enter_from_main(incoming) } -> std::same_as<void>;
    { sink.return_to_main(outgoing) } -> std::same_as<void>;
    { sink.reclaim(outgoing) } -> std::same_as<void>;
};

/**
 * @brief         Owns the ready queue and the current task.
 * @note          Constant-initialized state only, per the zero-
 *                construction rule; nothing runs until a sink is
 *                injected through init.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class Scheduler : public cinux::base::Singleton<Scheduler> {
    friend class cinux::base::Singleton<Scheduler>;

public:
    /**
     * @brief         Inject the executor that performs real switches.
     * @param[in]     sink   Any SwitchSink; stored type-erased.
     * @return        None.
     * @note          Called once during bring-up, kernel and every
     *                test world alike — the Tick init pattern one
     *                subsystem over.
     * @since         0.2.0
     * @ingroup       kernel_proc
     */
    template <SwitchSink Sink>
    void init(Sink& sink) {
        switch_tasks_ = [](void* ctx, Task& outgoing, Task& incoming) {
            static_cast<Sink*>(ctx)->switch_tasks(outgoing, incoming);
        };
        enter_from_main_ = [](void* ctx, Task& incoming) {
            static_cast<Sink*>(ctx)->enter_from_main(incoming);
        };
        return_to_main_ = [](void* ctx, Task& outgoing) {
            static_cast<Sink*>(ctx)->return_to_main(outgoing);
        };
        reclaim_ = [](void* ctx, Task& dead) { static_cast<Sink*>(ctx)->reclaim(dead); };
        sink_    = &sink;
    }

    /**
     * @brief         Hand one ready task to the queue.
     * @param[in,out] task   Armed task, state and pid get assigned.
     * @return        None.
     * @note          The seam tests drive directly: seat a hand-made
     *                task, install a recording sink, and the decision
     *                stream is observable without any machine. The
     *                kernel side pairs this with TaskBuilder, which
     *                owns the building half. The task queues on its
     *                own sched_class; a class-less task is assigned
     *                the first-registered one here, once.
     * @since         0.2.0
     * @ingroup       kernel_proc
     */
    void seat(Task& task);

    /**
     * @brief         Register a scheduling class; pick order follows
     *                registration order.
     * @param[in]     cls   The class to consult; kept by address.
     * @return        None.
     * @note          Registering the same class twice is a no-op, so
     *                bring-up paths can stay unguarded. Overflow past
     *                kMaxSchedulingClasses is a loud Check.
     * @since         0.6.0
     * @ingroup       kernel_proc
     */
    void install_class(SchedulingClass& cls);

    /**
     * @brief         How many classes stand registered.
     * @return        The registration count.
     * @since         0.6.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] unsigned int class_count() const;

    /**
     * @brief         One registered class, by registration order.
     * @param[in]     index   Position in registration order.
     * @return        The class, nullptr past the end.
     * @note          The registry read-back: registration order is the
     *                pick order, and this names the roster without
     *                reaching into private storage.
     * @since         0.6.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] SchedulingClass* class_at(unsigned int index) const;

    /**
     * @brief         Let another task run; the caller comes back later.
     * @return        None.
     * @note          Returns only when this task is picked again.
     *                Cooperative yield and preemption both land here;
     *                with preemption enabled, the clock rotates tasks
     *                that never call this.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    void yield();

    /**
     * @brief         Count one timer tick; arm a rotation request when
     *                the current task spent its slice.
     * @return        None.
     * @note          Called from the timer interrupt handler, which
     *                only keeps books: the switch itself happens later,
     *                in the interrupt-exit hook.
     * @since         0.3.0
     * @ingroup       kernel_proc
     */
    void on_timer_tick();

    /**
     * @brief         The interrupt-exit question: rotate now?
     * @return        None.
     * @note          Called with interrupts off, between the dispatch
     *                and the iret of every device interrupt. Rotates
     *                when a slice expired and preemption is enabled;
     *                the interrupted task resumes through iret with its
     *                flags restored, so the IF story needs no patching.
     * @since         0.3.0
     * @ingroup       kernel_proc
     */
    void maybe_preempt();

    /**
     * @brief         Open or close preemption.
     * @param[in]     enabled   True to let the clock rotate tasks.
     * @return        None.
     * @note          Ships off: the heap and the PMM are shared state
     *                without protection until the sync station arms
     *                them, so only run preemption demos whose tasks
     *                touch no shared structure. The guard count
     *                (PreemptGuard) also feeds off this switch.
     * @since         0.3.0
     * @ingroup       kernel_proc
     */
    void set_preemption(bool enabled);

    /**
     * @brief         Pin the current task to the CPU (one nest level).
     * @return        None.
     * @note          While any guard lives, the exit hook declines to
     *                rotate. The first consumer is the lock family of
     *                the next station; the heap wears it the day its
     *                allocator learns about preemption.
     * @since         0.3.0
     * @ingroup       kernel_proc
     */
    void preempt_disable();

    /**
     * @brief         Drop one nest level of pinning.
     * @return        None.
     * @note          Pairs with preempt_disable; the guard below is
     *                the way to call them so no path forgets the drop.
     * @since         0.3.0
     * @ingroup       kernel_proc
     */
    void preempt_enable();

    /**
     * @brief         End the current task; never returns to it.
     * @param[in]     code   The exit word ring 3 asked for, zero when
     *                       a kernel thread's entry merely returned.
     * @return        None.
     * @note          Reached through the trampoline's exit hook when
     *                an entry returns, or directly by choice. A task
     *                with a parent turns zombie and stays for that
     *                parent's wait; a parentless one goes straight to
     *                reclaim, executed after the switch. Either way
     *                the task's own offspring are settled first —
     *                zombie grandchildren reaped, live ones released
     *                to a parentless life — so no pointer outlives its
     *                target. Returning at all is only observable under
     *                a sink whose switches return — the recording sink
     *                of a host test; the kernel sink never does.
     * @since         0.2.0
     * @ingroup       kernel_proc
     */
    void exit_current(long long code = 0);

    /**
     * @brief         Wait for one child to exit and retire it.
     * @param[in]     wanted    Child pid, or -1 for any child.
     * @param[out]    receipt   The reaped child's pid and exit word.
     * @return        True when a child was reaped; false when the
     *                caller has no child left to wait for.
     * @note          Parks the caller until a matching zombie exists,
     *                the same park the lock family uses. A child's
     *                exit is the only wakeup, and it lands on the
     *                parent alone — the child knows exactly whose wait
     *                it finishes. Waking with no match simply parks
     *                again, so spurious wakeups cost nothing.
     * @since         0.2.0
     * @ingroup       kernel_proc
     */
    bool wait_child(long long wanted, ChildReceipt& receipt);

    /**
     * @brief         Park the current task off the ready queue and
     *                switch away; returns only after a wake.
     * @return        None.
     * @note          The blocking half of every lock: the caller is
     *                already linked onto the lock's wait queue when
     *                this runs. A spinlock held across this call
     *                stops the machine loudly — the rule the lock
     *                family is built on.
     * @since         0.4.0
     * @ingroup       kernel_proc
     */
    void block_current();

    /**
     * @brief         Wake one parked task into the ready queue.
     * @param[in,out] task   A blocked task, linked by its wait queue.
     * @return        None.
     * @note          The waking half; the lock transfers ownership
     *                before calling this so the woken task resumes
     *                already holding what it waited for.
     * @since         0.4.0
     * @ingroup       kernel_proc
     */
    void unblock(Task& task);

    /**
     * @brief         Hand the CPU to the ready queue until it drains.
     * @return        None.
     * @note          Back on kernel Main's stack when the last task
     *                has exited.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    void run_until_done();

    /**
     * @brief         The task on the CPU, nullptr on kernel Main.
     * @return        Current task or nullptr.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] Task* current() const;

    /**
     * @brief         How many tasks still stand in the offspring roll.
     * @return        The roll's headcount.
     * @note          The diagnosis face for the orphan story: a task
     *                joins the roll at seat because it has a parent,
     *                and the only way off is its parent's exit Current
     *                detaching it. When every family has finished its
     *                rounds, the count must be back where it started —
     *                anything left behind is a grandchild the detach
     *                walk stranded on the roll.
     * @since         0.5.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] unsigned long long offspring_count() const;

private:
    Scheduler() = default;


    [[nodiscard]] bool  sink_ready() const;
    void                reap_pending();
    void                rotate_from_interrupt();
    void                park_current();
    [[nodiscard]] Task* pick_next();
    Task*               take_zombie_child(Task& watcher, long long wanted, bool& has_any_child);
    void                detach_offspring(Task& dying);

    void (*switch_tasks_)(void*, Task&, Task&) = nullptr;
    void (*enter_from_main_)(void*, Task&)     = nullptr;
    void (*return_to_main_)(void*, Task&)      = nullptr;
    void (*reclaim_)(void*, Task&)             = nullptr;
    void* sink_                                = nullptr;

    cinux::base::container::SelfList<Task, &Task::child_link>                    offspring_;
    cinux::base::container::SelfList<Task>                                       waiters_;
    cinux::base::container::StaticStack<SchedulingClass*, kMaxSchedulingClasses> classes_ = {};
    Task*                                                                        current_ = nullptr;
    Task*              pending_reap_                                                      = nullptr;
    unsigned long long next_pid_                                                          = 1;
    unsigned int       slice_ticks_                                                       = 0;
    bool               rotate_due_                                                        = false;
    bool               preemption_                                                        = false;
    unsigned int       preempt_disable_depth_                                             = 0;
};

/**
 * @brief         Preemption discipline for the ScopedGuard shell.
 * @note          The counting lives in the scheduler; the guard shape
 *                lives in base. An empty State: this discipline needs
 *                no memory, only the depth.
 * @since         0.3.0
 * @ingroup       kernel_proc
 */
struct PreemptLockPolicy {
    using State = struct {};

    static void enter(State& /*state*/) { Scheduler::self().preempt_disable(); }

    static void exit(State& /*state*/) { Scheduler::self().preempt_enable(); }
};

/// RAII pin of the current task; one per scope, nests freely.
using PreemptGuard = cinux::base::ScopedGuard<PreemptLockPolicy>;

}  // namespace cinux::proc
