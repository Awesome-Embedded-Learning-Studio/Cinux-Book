/**
 * @file kernel/syscall/sys_futex.cpp
 * @brief futex syscall implementation (F3-M2 batch 2)
 *
 * 256-bucket hash table (keyed by the futex word vaddr) of intrusive FIFO
 * wait queues built on Task::wait_next + Spinlock -- the same block/unblock
 * handshake as kernel/proc/sync.cpp (Semaphore).  Supported ops:
 *   FUTEX_WAIT / FUTEX_WAKE              (bitset = all bits)
 *   FUTEX_WAIT_BITSET / FUTEX_WAKE_BITSET (bitset from val3)
 * Not supported: timeout, PI, requeue, robust list.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_futex.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // get_user for futex words
#include "kernel/errno.hpp"                     // kEagain/kEfault/kEinval/kEnosys (canonical int errno)
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::syscall {

using cinux::proc::Spinlock;
using cinux::proc::Task;

namespace {

// Futex op numbers (Linux subset).
constexpr int FUTEX_WAIT         = 0;
constexpr int FUTEX_WAKE         = 1;
constexpr int FUTEX_WAIT_BITSET  = 9;
constexpr int FUTEX_WAKE_BITSET  = 10;
constexpr int FUTEX_PRIVATE_FLAG = 0x80;

constexpr int      kFutexBuckets = 256;
constexpr uint32_t kFutexAllBits = 0xFFFFFFFFu;

struct FutexBucket {
    Spinlock lock;
    Task*    wait_head;
};

FutexBucket g_futex_table[kFutexBuckets];

/// Pick a bucket for @p uaddr.  Futex words are 4-byte aligned; shift out the
/// low bits to spread neighbouring words across buckets.
FutexBucket& bucket_for(uint64_t uaddr) {
    return g_futex_table[(uaddr >> 2) & (kFutexBuckets - 1)];
}

/// Append @p task to the tail of the bucket's wait queue.
void enqueue_waiter(FutexBucket& b, Task* task) {
    task->wait_next = nullptr;
    if (b.wait_head == nullptr) {
        b.wait_head = task;
        return;
    }
    Task* tail = b.wait_head;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}

/**
 * @brief Block the current task on @p uaddr until woken.
 *
 * Reads *uaddr under the bucket lock; if it differs from @p val returns
 * -EAGAIN.  Otherwise records the wait (uaddr + bitset) on the task, enqueues
 * it, then blocks.  FUTEX_WAKE unlinks the waiter from the queue before
 * unblocking, so on resume the task is already off the queue.
 */
int64_t futex_wait(uint64_t uaddr, uint32_t val, uint32_t bitset) {
    Task* self = cinux::proc::percpu()->current;
    if (self == nullptr) {
        return kEinval;
    }

    FutexBucket& b = bucket_for(uaddr);

    {
        // IRQ-safe (F4-M4 prepare-to-wait): the Blocked flip + enqueue must be
        // atomic vs a concurrent FUTEX_WAKE on another CPU, and no local tick may
        // preempt us while we hold the bucket lock.  The guard drops before switch.
        auto g = b.lock.irq_guard();
        uint32_t cur = 0;
        if (!cinux::user::get_user(&cur, reinterpret_cast<const uint32_t*>(uaddr))) {
            return -kEfault;
        }
        if (cur != val) {
            return -kEagain;  // guard drops
        }

        self->futex_uaddr  = uaddr;
        self->futex_bitset = bitset;
        enqueue_waiter(b, self);
        cinux::proc::Scheduler::prepare_to_wait(self);
    }  // guard drops: release bucket lock + restore IRQs, BEFORE switching out
    cinux::proc::Scheduler::schedule_blocked();
    return 0;
}

/**
 * @brief Wake up to @p max waiters on @p uaddr whose bitset intersects @p bitset.
 *
 * Unlinks matching waiters from the bucket (under the lock) into a local
 * list, then unblocks them outside the lock (same rule as Semaphore::post).
 */
int64_t futex_wake(uint64_t uaddr, uint32_t max, uint32_t bitset) {
    FutexBucket& b         = bucket_for(uaddr);
    Task*        wake_list = nullptr;
    uint32_t     woken     = 0;

    b.lock.acquire();
    Task** pp = &b.wait_head;
    while (*pp != nullptr && woken < max) {
        Task* t = *pp;
        if (t->futex_uaddr == uaddr && (t->futex_bitset & bitset) != 0) {
            *pp          = t->wait_next;  // unlink from bucket
            t->wait_next = wake_list;     // push onto local wake list
            wake_list    = t;
            ++woken;
        } else {
            pp = &t->wait_next;
        }
    }
    b.lock.release();

    while (wake_list != nullptr) {
        Task* next              = wake_list->wait_next;
        wake_list->wait_next    = nullptr;
        wake_list->futex_uaddr  = 0;
        wake_list->futex_bitset = 0;
        cinux::proc::Scheduler::unblock(wake_list);
        wake_list = next;
    }
    return static_cast<int64_t>(woken);
}

}  // namespace

int64_t futex_wake_addr(uint64_t uaddr, uint32_t max) {
    // Kernel-internal wake (all bits); used by CLONE_CHILD_CLEARTID's exit path.
    return futex_wake(uaddr, max, kFutexAllBits);
}

int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t /*timeout*/,
                  uint64_t /*uaddr2*/, uint64_t val3) {
    if (uaddr == 0) {
        return -kEfault;
    }

    const int base_op = static_cast<int>(op) & ~FUTEX_PRIVATE_FLAG;

    switch (base_op) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, static_cast<uint32_t>(val), kFutexAllBits);
    case FUTEX_WAKE:
        return futex_wake(uaddr, static_cast<uint32_t>(val), kFutexAllBits);
    case FUTEX_WAIT_BITSET: {
        const uint32_t bits = static_cast<uint32_t>(val3);
        if (bits == 0) {
            return -kEinval;  // zero bitset is invalid
        }
        return futex_wait(uaddr, static_cast<uint32_t>(val), bits);
    }
    case FUTEX_WAKE_BITSET: {
        const uint32_t bits = static_cast<uint32_t>(val3);
        if (bits == 0) {
            return -kEinval;
        }
        return futex_wake(uaddr, static_cast<uint32_t>(val), bits);
    }
    default:
        return -kEnosys;
    }
}

}  // namespace cinux::syscall
