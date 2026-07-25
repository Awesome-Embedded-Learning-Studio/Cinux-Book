/**
 * @file kernel/proc/timer_queue.cpp
 * @brief Timer-wake primitive implementation (F8-M5 / F5-M4 follow-up)
 *
 * A small fixed table of (Task*, deadline) entries, Spinlock-guarded.  arm()
 * records a deadline for a task; tick() (called from the PIT-driven
 * Scheduler::tick) wakes every task whose deadline passed.  No heap, no sorted
 * list -- a linear scan of kMaxTimers is cheap at hobby scale (a handful of
 * timed waits at once), and keeps the structure trivially correct.
 *
 * Lock discipline: the expired set is collected under the spinlock, then
 * Scheduler::unblock is called AFTER releasing it, so the (heavier) run-queue +
 * kprintf + IPI work of unblock never runs under the timer lock (lock order is
 * strictly timer_lock -> run-queue lock, never the reverse).
 */

#include "kernel/proc/timer_queue.hpp"

#include <stdint.h>

#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/drivers/pit/pit.hpp"  // monotonic fallback when HPET is absent
#include "kernel/proc/process.hpp"     // Task
#include "kernel/proc/scheduler.hpp"   // Scheduler::unblock
#include "kernel/proc/sync.hpp"        // Spinlock

namespace cinux::proc {

namespace {

/// Concurrent timed waits.  Plenty for a hobby OS (poll/select/nanosleep across
/// a few tasks); if exhausted, arm() returns false and the caller falls back to
/// sleeping on its wait queue without a timeout (never hangs).
constexpr uint32_t kMaxTimers = 32;

struct Entry {
    Task*    task;
    uint64_t deadline_ns;
    bool     armed;
};

Entry    g_entries[kMaxTimers];
Spinlock g_lock;

uint64_t monotonic_ns() {
    if (cinux::drivers::g_hpet.available()) {
        return cinux::drivers::g_hpet.monotonic_ns();
    }
    return cinux::drivers::PIT::get_uptime_ms() * 1'000'000ULL;
}

}  // namespace

bool timer_queue_arm(Task* task, uint64_t deadline_ns) {
    if (task == nullptr) {
        return false;
    }
    auto g = g_lock.irq_guard();
    // Replace an existing arm for this task (a re-park after a spurious wake).
    for (uint32_t i = 0; i < kMaxTimers; ++i) {
        if (g_entries[i].armed && g_entries[i].task == task) {
            g_entries[i].deadline_ns = deadline_ns;
            return true;
        }
    }
    for (uint32_t i = 0; i < kMaxTimers; ++i) {
        if (!g_entries[i].armed) {
            g_entries[i].task        = task;
            g_entries[i].deadline_ns = deadline_ns;
            g_entries[i].armed       = true;
            return true;
        }
    }
    return false;  // table full
}

void timer_queue_disarm(Task* task) {
    if (task == nullptr) {
        return;
    }
    auto g = g_lock.irq_guard();
    for (uint32_t i = 0; i < kMaxTimers; ++i) {
        if (g_entries[i].armed && g_entries[i].task == task) {
            g_entries[i].armed = false;
            g_entries[i].task  = nullptr;
        }
    }
}

void timer_queue_tick() {
    uint64_t now = monotonic_ns();

    Task*    fired[kMaxTimers];
    uint32_t nfired = 0;
    {
        auto g = g_lock.irq_guard();
        for (uint32_t i = 0; i < kMaxTimers; ++i) {
            if (g_entries[i].armed && g_entries[i].deadline_ns <= now) {
                fired[nfired++]    = g_entries[i].task;
                g_entries[i].armed = false;
                g_entries[i].task  = nullptr;
            }
        }
    }

    // Wake outside the timer lock (unblock takes the run-queue lock + may IPI).
    for (uint32_t i = 0; i < nfired; ++i) {
        if (fired[i] != nullptr) {
            Scheduler::unblock(fired[i]);
        }
    }
}

}  // namespace cinux::proc
