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
#include <type_traits>

#include "cinux/container/self_list.hpp"
#include "kernel/arch/x86_64/per_cpu_config.hpp"
#include "kernel/proc/checks_config.hpp"
#include "kernel/proc/lockdep.hpp"
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
    void note_acquire(const void* lock);

    /**
     * @brief     Record one spinlock scope dropped.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void note_release(const void* lock);

    /**
     * @brief     Live spinlock scopes right now.
     * @return    Nesting depth; zero means nothing is held.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    [[nodiscard]] unsigned int depth() const;

    /**
     * @brief     Register the human name of a lock on this ledger.
     * @since     0.4.0
     * @ingroup   kernel_proc
     */
    void name_lock(const void* lock, const char* label);

    /**
     * @brief     Walk this ledger's held chain for the dump face.
     * @since     0.4.0
     * @ingroup   kernel_proc
     */
    void dump_held();

    /**
     * @brief   Drop the identity of an unheld lock before storage reuse.
     * @since   0.1.0
     * @ingroup kernel_proc
     */
    void forget(const void* lock);

private:
    /// Mirrors the LockOrder faces the ledger forwards to, as no-ops,
    /// so the forwarding code compiles unchanged in dump-free builds.
    struct InactiveOrder {
        void name_lock(const void* /*lock*/, const char* /*label*/) {}
        void dump_held() {}
    };
    using Order = std::conditional_t<kLockdepEnabled, LockOrder, InactiveOrder>;
    unsigned int                depth_[arch::per_cpu::kMaxCpus] = {};
    [[no_unique_address]] Order order_{};
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
    /**
     * @brief   Acquire the supplied lock for this scope.
     * @param[in,out] lock Lock released when the scope ends.
     * @return  None.
     * @since   0.1.0
     * @ingroup kernel_proc
     */
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
     * @brief   Forget a temporary lock before its address is reused.
     * @return  None.
     * @note    Requires no holder; permanent locks need no retirement.
     * @since   0.1.0
     * @ingroup kernel_proc
     */
    void retire();

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

    /**
     * @brief     Give the lock its name in the ledger's registry.
     * @param[in] label   Caller-owned storage outliving the lock.
     * @note      Call once, before the first acquisition; the label is
     *            what the held-chain dump prints for this lock.
     * @since     0.4.0
     * @ingroup   kernel_proc
     */
    void set_name(const char* label);

private:
    friend class SpinGuardImpl;

    std::atomic_flag   held_;
    std::atomic<int>   owner_cpu_{-1};
    unsigned long long irq_state_ = 0;
    Task*              holder_    = nullptr;
    SpinLedger*        ledger_    = nullptr;
    const char*        name_      = nullptr;
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
    Spinlock                                                      spin_;
    Task*                                                         owner_ = nullptr;
    cinux::base::container::SelfList<Task, &Task::sync_wait_link> waiters_;
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
     * @brief Forget this semaphore's lock after all users have left.
     * @return None.
     * @since 0.3.0
     * @ingroup kernel_proc
     */
    void retire();

    /**
     * @brief     Consume a unit, or park until one arrives.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void wait();

    /**
     * @brief Consumes an available unit without parking.
     * @return True when one unit was acquired.
     * @since 0.8.0
     * @ingroup kernel_proc
     */
    bool try_wait();


    /**
     * @brief     Wake the head waiter or bank a unit.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void post();

private:
    Spinlock                                                      spin_;
    long long                                                     count_ = 0;
    cinux::base::container::SelfList<Task, &Task::sync_wait_link> waiters_;
};

/**
 * @brief         One request's completion: a one-shot state, no tokens.
 * @note          finish() moves Pending to a terminal state exactly
 *                once — later calls change nothing — so an unexpected
 *                extra event cannot make the next waiter leave early
 *                the way a surplus semaphore token could. wait() parks
 *                a task until the terminal state; earlier contexts read
 *                finished() and success() instead of waiting.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class Completion {
public:
    constexpr Completion() = default;

    /**
     * @brief     Moves the state to its terminal form, once.
     * @param[in] success   The verdict to publish.
     * @return    True when this call made the transition.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    bool finish(bool success);

    /**
     * @brief     Parks the current task until a terminal state.
     * @return    The published verdict.
     * @note      Task contexts only; a stocked state returns without
     *            parking.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    bool wait();

    /// @brief Whether a terminal state arrived; safe from any context.
    [[nodiscard]] bool finished() const;

    /// @brief The published verdict; read after finished() turns true.
    [[nodiscard]] bool success() const;

    /**
     * @brief Releases the completed rendezvous's lock identity.
     * @return None.
     * @since 0.8.0
     * @ingroup kernel_proc
     */
    void retire();

    /**
     * @brief     Returns a terminal completion to Pending.
     * @note      For request pools that recycle slots: a still-pending
     *            completion stays pending, a terminal one returns to
     *                    pending, and any parked waiter refuses the
     *                    call loudly.
     * @since     0.1.0
     * @ingroup   kernel_proc
     */
    void rearm();

private:
    enum class State : unsigned char {
        kPending,
        kSuccess,
        kFailed,
    };

    mutable Spinlock                                              spin_;
    State                                                         state_ = State::kPending;
    cinux::base::container::SelfList<Task, &Task::sync_wait_link> waiters_;
};

}  // namespace cinux::proc
