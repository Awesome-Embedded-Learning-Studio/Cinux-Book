/**
 * @file kernel/net/wait_queue.hpp
 * @brief Intrusive FIFO wait queue built on Task::wait_next
 *
 * Shared wait/wake helpers used by blocking fd types (pipe ends, file locks).
 * The queue is a singly-linked list threaded through Task::wait_next; no heap
 * allocation.  Wakeups call Scheduler::unblock; callers drive the
 * prepare_to_wait / schedule_blocked park themselves (under the right lock, so
 * no lost wakeup).
 */

#pragma once

#include "kernel/proc/scheduler.hpp"  // Scheduler::unblock
#include "kernel/proc/process.hpp"    // Task

namespace cinux::net {

/// Append @p t to the tail of a wait queue (intrusive via Task::wait_next).
inline void wait_enqueue(cinux::proc::Task*& head, cinux::proc::Task* t) {
    t->wait_next = nullptr;
    if (head == nullptr) {
        head = t;
        return;
    }
    cinux::proc::Task* tail = head;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = t;
}

/// Remove and return the queue head, or nullptr if empty.
inline cinux::proc::Task* wait_dequeue(cinux::proc::Task*& head) {
    cinux::proc::Task* t = head;
    if (t != nullptr) {
        head         = t->wait_next;
        t->wait_next = nullptr;
    }
    return t;
}

/// Wake one waiter (FIFO head).  Called under lock_; safe because no ownership
/// is transferred -- the woken task re-acquires its lock fresh when scheduled.
inline void wake_one(cinux::proc::Task*& head) {
    cinux::proc::Task* t = wait_dequeue(head);
    if (t != nullptr) {
        cinux::proc::Scheduler::unblock(t);
    }
}

/// Wake every waiter (used on close / release so blocked peers observe it).
inline void wake_all(cinux::proc::Task*& head) {
    while (cinux::proc::Task* t = wait_dequeue(head)) {
        cinux::proc::Scheduler::unblock(t);
    }
}

}  // namespace cinux::net
