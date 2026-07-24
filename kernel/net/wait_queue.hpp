/**
 * @file kernel/net/wait_queue.hpp
 * @brief Intrusive singly-linked wait queue shared by the socket implementations.
 *
 * Tcp/Udp/Unix sockets all block the same way: a recv'er / accept'er that would
 * wait parks on a Task* intrusive list; the producer wake_one()/wake_all()s it;
 * poll detach wait_remove()s it.  Formerly byte-for-byte duplicated across
 * tcp_socket.cpp / udp_socket.cpp / unix_socket.cpp.
 *
 * Host unit tests (CINUX_HOST_TEST) compile the blocking path out, so the whole
 * header collapses to nothing in that configuration.
 *
 * Namespace: cinux::net
 */

#pragma once

#ifndef CINUX_HOST_TEST
#    include "kernel/proc/process.hpp"    // cinux::proc::Task::wait_next
#    include "kernel/proc/scheduler.hpp"  // cinux::proc::Scheduler::unblock
#endif

namespace cinux::net {

#ifndef CINUX_HOST_TEST

inline void wait_enqueue(cinux::proc::Task*& head, cinux::proc::Task* t) {
    t->wait_next = nullptr;
    // Record the queue head address so signal_send() can wake us for EINTR
    // (see Task::wait_queue_head).  Cleared on dequeue/remove/wake.
    t->wait_queue_head = &head;
    if (head == nullptr) {
        head = t;
        return;
    }
    cinux::proc::Task* x = head;
    while (x->wait_next != nullptr) {
        x = x->wait_next;
    }
    x->wait_next = t;
}

inline cinux::proc::Task* wait_dequeue(cinux::proc::Task*& head) {
    cinux::proc::Task* t = head;
    if (t != nullptr) {
        head         = t->wait_next;
        t->wait_next = nullptr;
        t->wait_queue_head = nullptr;  // no longer queued
    }
    return t;
}

inline void wake_one(cinux::proc::Task*& head) {
    if (cinux::proc::Task* t = wait_dequeue(head)) {
        cinux::proc::Scheduler::unblock(t);
    }
}

inline void wake_all(cinux::proc::Task*& head) {
    while (cinux::proc::Task* t = wait_dequeue(head)) {
        cinux::proc::Scheduler::unblock(t);
    }
}

/// Unlink @p t from the wait queue (poll detach, or a signal-woken task
/// unlinking itself after EINTR).  No-op if not queued.
inline void wait_remove(cinux::proc::Task*& head, cinux::proc::Task* t) {
    if (head == nullptr || t == nullptr) {
        return;
    }
    if (head == t) {
        head         = t->wait_next;
        t->wait_next = nullptr;
        t->wait_queue_head = nullptr;
        return;
    }
    cinux::proc::Task* prev = head;
    while (prev->wait_next != nullptr && prev->wait_next != t) {
        prev = prev->wait_next;
    }
    if (prev->wait_next == t) {
        prev->wait_next = t->wait_next;
        t->wait_next    = nullptr;
        t->wait_queue_head = nullptr;
    }
}

#endif  // CINUX_HOST_TEST

}  // namespace cinux::net
