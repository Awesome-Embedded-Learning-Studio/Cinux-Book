/**
 * @file kernel/mm/diagnostics.hpp
 * @brief Kernel memory-state summary for panic dumps and ad-hoc diagnostics
 *
 * dump_memory_stats() prints a one-shot snapshot of the allocator hierarchy
 * (PMM free/total pages, mapped slab pages, page-cache occupancy + hit rate)
 * to all kprintf sinks.  The panic handler calls it (FO batch 4) so a crash
 * report shows how much memory was free and how warm the caches were.
 *
 * Namespace: cinux::mm
 */

#pragma once

namespace cinux::mm {

/**
 * @brief Print a one-line-per-subsystem memory summary.
 *
 * Safe at any init stage: prints zeros before the subsystems are initialised
 * (the globals exist, their counters merely read as zero).
 */
void dump_memory_stats();

/**
 * @brief Spawn the periodic memory-stats kthread (CINUX_STATS_KTHREAD=ON only).
 *
 * Resident low-priority kthread calling dump_memory_stats() every ~1 s, paced
 * by the HPET counter.  For ad-hoc profiling (e.g. gcc-compile-stutter).  When
 * CINUX_STATS_KTHREAD=OFF (the default), stats_kthread_stub.cpp supplies an
 * empty body, so callers (init.cpp) need no #ifdef (§14 file gate).
 */
void start_stats_thread();

}  // namespace cinux::mm
