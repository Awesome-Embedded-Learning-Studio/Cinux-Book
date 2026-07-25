/**
 * @file kernel/proc/timer_queue.hpp
 * @brief Timer-wake primitive: wake a Blocked task at a monotonic deadline.
 *
 * The scheduler could block a task on a wait queue (prepare_to_wait +
 * schedule_blocked) but had no way to wake it after a TIMEOUT -- sys_nanosleep
 * yield-polled the HPET counter, and a finite-timeout poll/select would have had
 * to do the same (burning CPU while idle).  This module adds the missing half:
 * arm a Blocked task with an absolute deadline; the periodic scheduler tick
 * (PIT-driven) scans the armed timers and unblocks any whose deadline passed.
 *
 * Usage from a wait path (e.g. do_poll_core):
 *   1. under InterruptGuard: prepare_to_wait(self); ... register on fd queues;
 *      timer_queue_arm(self, deadline);   // before schedule_blocked
 *   2. schedule_blocked();                // parks; woken by a fd OR the timer
 *   3. after wake: detach fd queues; timer_queue_disarm(self);
 * Either wake source calls Scheduler::unblock, which is idempotent, so the
 * second is a harmless no-op.  This finally makes a finite-timeout poll/select
 * a true park (no spin), and is the F5-M4 timer follow-up.
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stdint.h>

namespace cinux::proc {

struct Task;

/**
 * @brief Arm @p task to be woken (Scheduler::unblock) once the monotonic clock
 *        reaches @p deadline_ns.
 *
 * Replaces any prior arm for @p task.  Call BEFORE schedule_blocked(), under the
 * same IRQ-off window as prepare_to_wait, so no tick can fire between arming and
 * parking (no missed wakeup).  @return true if armed; false if the table is full
 * (the caller should treat a full table as "no timeout" rather than hang).
 */
bool timer_queue_arm(Task* task, uint64_t deadline_ns);

/**
 * @brief Cancel a pending timer-wake for @p task (no-op if not armed).
 *
 * Call after the task wakes -- whether by the timer or by a fd event -- so the
 * entry is freed for reuse and a later tick does not fire a stale wakeup.
 */
void timer_queue_disarm(Task* task);

/**
 * @brief Advance the queue: unblock every armed task whose deadline has passed.
 *
 * Called from the periodic scheduler tick (Scheduler::tick, PIT-driven).  Safe
 * in IRQ context (unblock is).  Cheap when empty (one locked scan of a small
 * fixed table).
 */
void timer_queue_tick();

}  // namespace cinux::proc
