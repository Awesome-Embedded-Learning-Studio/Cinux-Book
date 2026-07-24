/**
 * @file kernel/mm/stats_kthread.cpp
 * @brief Periodic memory-stats printer for ad-hoc profiling (B1 gcc-compile-stutter)
 *
 * Spawns a resident kthread that calls dump_memory_stats() every ~1 s (paced by
 * the HPET free-running counter).  The goal is a serial-log curve of PMM free
 * pages / slab pages / PageCache occupancy / cumulative #PF during a workload
 * -- e.g. `g++ hello.cpp` -- so a stutter's root cause (page_cache grow-only,
 * demand-page storm, slab pressure, ...) can be read off the trend instead of
 * guessed.
 *
 * Scheduling (the hard part -- the gate pegged at the first fork for two
 * earlier attempts): priority is "lower runs first", TaskBuilder default 0,
 * inherited by /sbin/init and every fork/exec'd cc1 -- so all user code sits in
 * band 0.  The kthread must ALSO be band 0:
 *   - Lower (e.g. 250, near idle 255) STARVES under a CPU-bound compile -- the
 *     exact workload we want to observe -- so no samples ever land.
 *   - Higher would preempt user code and skew the very numbers we measure.
 *   - And within band 0, do NOT sti/hlt: halting inside a band-0 thread makes
 *     the tick IRQ resume us (quantum not exhausted) and every other band-0
 *     task (init / fork / exec) waits forever -- the gate freezes at the first
 *     fork with #PF stuck at +0.  yield() instead: it hands the CPU to the next
 *     band-0 task, the tick preempts it back to us, and we re-check the HPET
 *     deadline each wake (~once per 10 ms tick -- well below the 1 s dump).
 *
 * Built only when CINUX_STATS_KTHREAD=ON; otherwise stats_kthread_stub.cpp
 * supplies an empty start_stats_thread() (§14 file gate).
 *
 * Namespace: cinux::mm
 */

#include <stdint.h>

#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/diagnostics.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task_builder.hpp"

namespace cinux::mm {

using cinux::lib::kprintf;

// 1 s in nanoseconds.  Coarse on purpose: a compile takes tens of seconds, so a
// ~1 Hz sample yields plenty of points without flooding the serial log.
constexpr uint64_t kStatsIntervalNs = 1'000'000'000;

void stats_thread_entry() {
    kprintf("[MEM] stats thread entry running\n");
    const bool have_hpet     = cinux::drivers::g_hpet.available();
    uint64_t   next_deadline = cinux::drivers::g_hpet.monotonic_ns() + kStatsIntervalNs;
    int        ticks_since_dump = 0;

    while (true) {
        // yield() hands the CPU to the next band-0 task (init/cc1/g++); the PIT
        // tick preempts it back to us, so we wake ~once per tick (~10 ms) --
        // far below the 1 s dump interval.  See file header for why this is NOT
        // sti/hlt and NOT a lower priority.
        cinux::proc::Scheduler::yield();

        bool due = false;
        if (have_hpet) {
            due = cinux::drivers::g_hpet.monotonic_ns() >= next_deadline;
            if (due) {
                next_deadline += kStatsIntervalNs;
            }
        } else {
            // No HPET: fall back to a tick count (~100 Hz PIT => ~1 s).
            due = (++ticks_since_dump >= 100);
            if (due) {
                ticks_since_dump = 0;
            }
        }
        if (due) {
            dump_memory_stats();
        }
    }
}

void start_stats_thread() {
    // priority 0 = the TaskBuilder default, same band as kernel_init and the
    // fork'd user processes.  See file header: lower starves, higher skews.
    auto* t = cinux::proc::TaskBuilder()
                  .set_entry(stats_thread_entry)
                  .set_name("mm_stats")
                  .set_priority(0)
                  .build();
    if (t != nullptr) {
        cinux::proc::Scheduler::add_task(t);
        kprintf("[MEM] periodic stats thread started (%llu ms interval)\n",
                static_cast<unsigned long long>(kStatsIntervalNs / 1'000'000));
    }
}

}  // namespace cinux::mm
