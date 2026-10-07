/**
 * @file    sync.hpp
 * @brief   The lock family: one spin, two sleeps.
 *
 * Spinlock is the fast lane — interrupts off, a test-and-set and a
 * pause — and everything else rides on it: Mutex and Semaphore guard
 * their metadata with it for the blink of a queue update, release
 * before sleeping, and park on an intrusive wait list. The blocking
 * pair is the scheduler's block_current/unblock; ownership transfers
 * to the woken waiter before the wake, so a Mutex holder resumes
 * already holding and never re-races. Observable by design, per the
 * station's rule: a spinlock records its holder, unlock verifies the
 * same hand lets go, lock refuses re-entry on one core, and sleeping
 * with a spinlock held stops the machine loudly instead of deadlocking
 * it quietly.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <atomic>

#include "cinux/container/self_list.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

class Spinlock;
class Mutex;
class Semaphore;

/**
 * @brief         The registry of live spinlock scopes on this core.
 * @note          A small instance class, not loose functions, so a
 *                test world can feed its own ledger to a lock and
 *                stay invisible to the scheduler's sleep check,
 *                which consults the shared default. depth() naming
 *                zero is the "safe to park" answer.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class SpinLedger {
public:
    /**
     * @brief     Record one more live spinlock scope.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void note_acquire() { depth_++; }

    /**
     * @brief     Record one spinlock scope dropped.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void note_release() { depth_--; }

    /**
     * @brief     Live spinlock scopes right now.
     * @return    Nesting depth; zero means nothing is held.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    [[nodiscard]] unsigned int depth() const { return depth_; }

private:
    unsigned int depth_ = 0;
};

/**
 * @brief         The shared ledger the kernel's locks run on.
 * @return        Reference to the one default instance.
 * @note          Locks default to this one; the scheduler's
 *                sleep-permission check reads it. Test-made ledgers ride
 *                their own instances and never disturb the check.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
SpinLedger& DefaultSpinLedger();

/**
 * @brief         RAII scope over one Spinlock.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class SpinGuard {
public:
    explicit SpinGuard(Spinlock& lock);
    ~SpinGuard();

    SpinGuard(const SpinGuard&)            = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;

private:
    Spinlock& lock_;
};

/**
 * @brief         RAII scope over one Mutex.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class MutexGuard {
public:
    explicit MutexGuard(Mutex& mutex);
    ~MutexGuard();

    MutexGuard(const MutexGuard&)            = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    Mutex& mutex_;
};

/**
 * @brief         Interrupt-blocking mutual exclusion, the fast lane.
 * @note          Saving IF on the way in and restoring on the way out
 *                is part of the deal: on one core, holding this means
 *                nobody else runs and nothing interrupts, which is
 *                what short metadata stretches want. Re-entry is a
 *                Check failure (it would spin on a flag already set
 *                with interrupts off), unlock by a stranger likewise.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class Spinlock {
public:
    /**
     * @brief         Bind this lock to a ledger.
     * @param[in,out] ledger   Which registry counts this lock's
     *                        scopes; defaults to the shared one.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    constexpr explicit Spinlock(SpinLedger* ledger = nullptr) : ledger_(ledger) {}

    /**
     * @brief     Take the lock; interrupts off until unlock.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void lock();

    /**
     * @brief     Give the lock back; interrupts return to the state
     *            lock found.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void unlock();

    /**
     * @brief     RAII pair for lock/unlock.
     * @return    Guard holding the lock for its scope.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    [[nodiscard]] SpinGuard guard();

    /**
     * @brief     Who holds this lock, for dumps and death reports.
     * @return    Holder task name, or "irq/main" outside tasks.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    [[nodiscard]] const char* holder_name() const;

private:
    friend class SpinGuardImpl;

    std::atomic_flag   held_;
    unsigned long long irq_state_ = 0;
    Task*              holder_    = nullptr;
    SpinLedger*        ledger_    = nullptr;
};

/**
 * @brief         Blocking mutual exclusion over a wait list.
 * @note          Ownership transfer on wake: unlock hands the lock to
 *                the head waiter and wakes it, so the woken task
 *                resumes holding and nobody re-races for the flag.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class Mutex {
public:
    /**
     * @brief     Acquire; parks on the wait list when contended.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void lock();

    /**
     * @brief     Release to the next waiter, or to nobody.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void unlock();

    /**
     * @brief     RAII pair for lock/unlock.
     * @return    Guard holding the mutex for its scope.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    [[nodiscard]] MutexGuard guard();

private:
    Spinlock                               spin_;
    Task*                                  owner_ = nullptr;
    cinux::base::container::SelfList<Task> waiters_;
};

/**
 * @brief         Counting semaphore: waiters sleep, posts wake.
 * @note          Classic counting shape: wait consumes a unit or
 *                parks; post wakes the head waiter (unit transfers,
 *                no count bounce) or banks a unit for the future.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class Semaphore {
public:
    /**
     * @brief         Start the count here.
     * @param[in]     initial   Units available before anyone waits.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    constexpr Semaphore() = default;

    explicit Semaphore(long long initial);

    /**
     * @brief     Consume a unit, or park until one arrives.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void wait();

    /**
     * @brief     Wake the head waiter or bank a unit.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void post();

private:
    Spinlock                               spin_;
    long long                              count_ = 0;
    cinux::base::container::SelfList<Task> waiters_;
};

}  // namespace cinux::proc
