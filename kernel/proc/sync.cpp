/**
 * @file kernel/proc/sync.cpp
 * @brief Implementation of synchronization primitives (Spinlock, Mutex, Semaphore)
 */

#include "kernel/proc/sync.hpp"

#include <stdint.h>

#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

// ============================================================
// Spinlock implementation
// ============================================================

#ifdef CINUX_LOCKDEP
// F-INFRA I-10: depth of held spinlocks on this CPU. Asserted at schedule().
uint32_t g_lockdep_held_depth = 0;
#endif

void Spinlock::acquire() {
    while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
#ifdef CINUX_LOCKDEP
    ++g_lockdep_held_depth;  // count only AFTER the lock is actually held
#endif
}

void Spinlock::release() {
#ifdef CINUX_LOCKDEP
    --g_lockdep_held_depth;  // decrement before the lock is freed
#endif
    __atomic_clear(&locked_, __ATOMIC_RELEASE);
}

Spinlock::IrqGuard::IrqGuard(Spinlock* lock) : lock_(lock) {
    __asm__ volatile("pushfq; popq %0; cli" : "=rm"(saved_flags_));
    lock_->acquire();
}

Spinlock::IrqGuard::~IrqGuard() {
    lock_->release();
    __asm__ volatile("pushq %0; popfq" : : "rm"(saved_flags_));
}

// ============================================================
// Mutex implementation
// ============================================================

void Mutex::enqueue_waiter(Task* task) {
    task->wait_next = nullptr;

    if (wait_head_ == nullptr) {
        wait_head_ = task;
        return;
    }

    Task* tail = wait_head_;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}

Task* Mutex::dequeue_waiter() {
    if (wait_head_ == nullptr) {
        return nullptr;
    }

    Task* task      = wait_head_;
    wait_head_      = task->wait_next;
    task->wait_next = nullptr;
    return task;
}

void Mutex::lock() {
    Task* self = percpu()->current;
    // Hold the metadata spinlock with IRQs disabled (F4-M4 prepare-to-wait): the
    // state flip to Blocked + enqueue onto the wait queue must be atomic vs a
    // concurrent unlock() on another CPU, and no local tick may preempt us while
    // we hold the lock.  The guard drops (re-enables IRQs) before we switch out.
    {
        auto g = spin_.irq_guard();

        // Fast path: mutex is free -- take ownership and return (guard drops).
        if (owner_ == nullptr) {
            owner_ = self;
            return;
        }

        // Contended: enqueue + mark ourselves Blocked under the lock.  A concurrent
        // unlock() racing through the window finds us already Blocked and unblocks
        // us; schedule()'s next==prev path then keeps us running -- no lost wakeup.
        enqueue_waiter(self);
        Scheduler::prepare_to_wait(self);
    }  // guard drops: release spin + restore IRQs, BEFORE switching out
    Scheduler::schedule_blocked();
}

void Mutex::unlock() {
    // Step 1: Acquire the internal spinlock
    spin_.acquire();

    // Step 2: If there is no waiter, simply release the mutex
    Task* waiter = dequeue_waiter();
    if (waiter == nullptr) {
        owner_ = nullptr;
        spin_.release();
        return;
    }

    // Step 3: Transfer ownership to the head waiter
    owner_ = waiter;

    // Step 4: Release the spinlock before unblocking
    spin_.release();

    // Step 5: Wake the new owner
    Scheduler::unblock(waiter);
}

bool Mutex::try_lock() {
    spin_.acquire();

    if (owner_ != nullptr) {
        spin_.release();
        return false;
    }

    owner_ = percpu()->current;
    spin_.release();
    return true;
}

// ============================================================
// Semaphore implementation
// ============================================================

Semaphore::Semaphore(int64_t initial) : count_(initial), wait_head_(nullptr) {}

void Semaphore::enqueue_waiter(Task* task) {
    task->wait_next = nullptr;

    if (wait_head_ == nullptr) {
        wait_head_ = task;
        return;
    }

    Task* tail = wait_head_;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}

Task* Semaphore::dequeue_waiter() {
    if (wait_head_ == nullptr) {
        return nullptr;
    }

    Task* task      = wait_head_;
    wait_head_      = task->wait_next;
    task->wait_next = nullptr;
    return task;
}

void Semaphore::post() {
    // Step 1: Acquire the internal spinlock
    spin_.acquire();

    // Step 2: Increment the count
    count_++;

    // Step 3: If there are waiters, wake the head
    Task* waiter = dequeue_waiter();

    // Step 4: Release spinlock before unblocking
    spin_.release();

    // Step 5: Unblock the waiter (if any)
    if (waiter != nullptr) {
        Scheduler::unblock(waiter);
    }
}

void Semaphore::wait() {
    Task* self = percpu()->current;
    {
        // IRQ-safe (F4-M4 prepare-to-wait): the Blocked flip + enqueue must be
        // atomic vs a concurrent post() on another CPU, and no local tick may
        // preempt us while we hold the lock.  The guard drops before we switch.
        auto g = spin_.irq_guard();
        count_--;

        // Resource available: return immediately (guard drops).
        if (count_ >= 0) {
            return;
        }

        enqueue_waiter(self);
        Scheduler::prepare_to_wait(self);
    }  // guard drops: release spin + restore IRQs, BEFORE switching out
    Scheduler::schedule_blocked();
}

bool Semaphore::try_wait() {
    spin_.acquire();

    if (count_ <= 0) {
        spin_.release();
        return false;
    }

    count_--;
    spin_.release();
    return true;
}

int64_t Semaphore::count() const {
    return count_;
}

// ============================================================
// InterruptGuard implementation
// ============================================================

InterruptGuard::InterruptGuard() {
    __asm__ volatile("pushfq; popq %0; cli" : "=rm"(saved_flags_));
}

InterruptGuard::~InterruptGuard() {
    __asm__ volatile("pushq %0; popfq" : : "rm"(saved_flags_));
}

}  // namespace cinux::proc
