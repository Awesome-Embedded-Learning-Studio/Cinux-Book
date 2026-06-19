/**
 * @file kernel/syscall/sys_futex.hpp
 * @brief futex syscall handler (F3-M2 batch 2)
 *
 * FUTEX_WAIT / FUTEX_WAKE / FUTEX_*_BITSET.  Backed by a 256-bucket hash
 * table keyed by the futex word's virtual address; each bucket is an
 * intrusive FIFO wait queue (Task::wait_next) guarded by a Spinlock,
 * mirroring kernel/proc/sync.cpp (Mutex/Semaphore).  No timeout / PI /
 * requeue yet.
 *
 * The handler reads user memory directly: the kernel maps the caller's
 * address space, so a user pointer is accessible without a copy primitive
 * (a bad pointer faults via the PF path) -- same convention as sys_signal.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Fast user-space mutex (Linux syscall 202)
 *
 * @param uaddr    futex word, 4-byte aligned user pointer
 * @param op       FUTEX_WAIT / FUTEX_WAKE / FUTEX_*_BITSET
 *                 (FUTEX_PRIVATE_FLAG is ignored)
 * @param val      expected value (WAIT) / max waiters to wake (WAKE)
 * @param timeout  unused -- no timeout support yet (treated as infinite)
 * @param uaddr2   unused (no requeue)
 * @param val3     bitset for FUTEX_*_BITSET
 * @return WAIT: 0 on wake, -EAGAIN if *uaddr!=val, -EINVAL/-EFAULT on bad args;
 *         WAKE: number of waiters woken; unsupported op: -ENOSYS
 */
int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout, uint64_t uaddr2,
                  uint64_t val3);

/**
 * @brief Wake up to @p max waiters on futex word @p uaddr (all bits).
 *
 * Kernel-internal wake used by CLONE_CHILD_CLEARTID's exit path (F3-M2 batch 5):
 * when a thread exits it zeros its child_tid and wakes any pthread_join waiter.
 * Same logic as FUTEX_WAKE without the syscall boundary.
 *
 * @return number of waiters woken.
 */
int64_t futex_wake_addr(uint64_t uaddr, uint32_t max);

}  // namespace cinux::syscall
