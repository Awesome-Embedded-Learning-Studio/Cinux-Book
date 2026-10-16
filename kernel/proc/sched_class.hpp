/**
 * @file    sched_class.hpp
 * @brief   The scheduling-class seam: policies own their queues.
 *
 * The scheduler used to be one hard-coded FIFO ring: who runs next was
 * whichever task pop returned. This seam lifts that decision out — a
 * SchedulingClass owns its own ready queue and answers three questions
 * (take this task, give me your best, has your idea of a slice run
 * out), while the scheduler keeps only the registry: classes are
 * consulted in registration order and the first non-empty hand wins.
 * A task without an explicit class rides the first-registered one, so
 * the plain FIFO world keeps running unchanged until a station needs
 * more.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::proc {

struct Task;

/**
 * @brief         One runnable-policy: a queue and the decisions over it.
 * @note          The scheduler never looks inside a class; it enqueues
 *                through the task's own class and picks by asking each
 *                registered class in turn. Implementations keep their
 *                queues in their own storage and must answer pick()
 *                without blocking or allocating.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class SchedulingClass {
public:
    /**
     * @brief         The class's name, as dumps and panic lines spell it.
     * @return        A stable, caller-owned string.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] virtual const char* name() const = 0;

    /**
     * @brief         Take one newly runnable task onto this class's queue.
     * @param[in,out] task   A task whose sched_class points here.
     * @return        True when seated; false when the class's queue is
     *                full — the scheduler turns that into a loud Check,
     *                mirroring the old ready-queue rule.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    virtual bool enqueue(Task& task) = 0;

    /**
     * @brief         Hand over the next task this class wants on the CPU.
     * @return        The chosen task, removed from the queue; nullptr
     *                when the class has nothing ready.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] virtual Task* pick() = 0;

    /**
     * @brief         Take a queued task back off this class's queue.
     * @param[in,out] task   A task previously seated here.
     * @return        True when it was found and removed; false when
     *                this class does not currently hold it.
     * @note          The stop-signal door: SIGSTOP withdraws a task
     *                from the runnable world, and only the class that
     *                queued it knows where it sits. Classes own their
     *                queues, so withdrawal is theirs to answer, the
     *                same ownership that makes pick() theirs.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    virtual bool remove(Task& task) = 0;

    /**
     * @brief         Whether the running task has spent its slice here.
     * @param[in]     slice_ticks   Ticks the current task has run, as
     *                                counted by the scheduler.
     * @return        True when the clock should ask for a rotation.
     * @note          The policy half of preemption: the scheduler only
     *                books ticks and flips its rotation flag on a true
     *                answer, the class says what a slice means.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] virtual bool wants_rotate(unsigned int slice_ticks) const = 0;

protected:
    /**
     * @brief         Classes are process-lifetime singletons, never
     *                deleted through this base.
     * @note          A protected non-virtual destructor: destruction
     *                always runs through the concrete class, and no
     *                caller can `delete` a SchedulingClass* — the
     *                registry keeps addresses, never ownership.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    ~SchedulingClass() = default;
};

/**
 * @brief         The plain FIFO class: first in, first on the CPU.
 * @return        The shared default instance.
 * @note          Register it first and every class-less task lands
 *                here, which is exactly the old scheduler's behavior.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] SchedulingClass& DefaultFifoClass();

/**
 * @brief         The priority class: smaller task priority runs first.
 * @return        The shared priority instance.
 * @note          Register it after the default class and only tasks
 *                that name it explicitly queue here. Ties break to the
 *                oldest arrival; nothing is aged or boosted — a steady
 *                stream of high-priority work can starve the low end,
 *                and that is told as it is.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] SchedulingClass& PriorityClass();

}  // namespace cinux::proc
