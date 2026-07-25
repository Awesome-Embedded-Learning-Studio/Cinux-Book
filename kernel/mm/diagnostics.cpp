/**
 * @file kernel/mm/diagnostics.cpp
 * @brief Memory-state summary implementation
 *
 * Aggregates the public stats of each MM subsystem into a compact dump.
 * kprintf's vkprintf_impl supports %u (unsigned) and %llu (unsigned long long)
 * with %0Nd zero-padding, so every counter is cast accordingly.
 */

#include "kernel/mm/diagnostics.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/fault_diag.hpp"  // pf_count (B1 gcc-stutter profiling)
#include "kernel/drivers/hpet/hpet.hpp"       // g_hpet.monotonic_ns for dump timestamp
#include "libs/ext2/ext2.hpp"            // ext2_read_* (B2.5 I/O accounting)
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/slab.hpp"

namespace cinux::mm {

using cinux::lib::kprintf;

void dump_memory_stats() {
    const uint64_t free_pages  = g_pmm.free_page_count();
    const uint64_t total_pages = g_pmm.total_page_count();
    const uint64_t pf_total    = pf_count();

    // Boot-relative timestamp so the periodic stats log is a curve with a real
    // time axis (HPET monotonic ns), letting a reader align [MEM] samples to
    // workload phases (e.g. the g++ compile window).  "0.000 s" before HPET init.
    const uint64_t now_ns =
        cinux::drivers::g_hpet.available() ? cinux::drivers::g_hpet.monotonic_ns() : 0;
    kprintf("[MEM] === t=%llu.%03u s ===\n",
            static_cast<unsigned long long>(now_ns / 1'000'000'000),
            static_cast<unsigned>((now_ns / 1'000'000) % 1000));

    kprintf("[MEM] PMM:       %u / %u pages free (%u KiB free of %u KiB)\n",
            static_cast<unsigned>(free_pages), static_cast<unsigned>(total_pages),
            static_cast<unsigned>(free_pages * 4), static_cast<unsigned>(total_pages * 4));
    kprintf("[MEM] Slab:      %u slab pages mapped\n",
            static_cast<unsigned>(g_slab.total_slab_pages()));
    kprintf("[MEM] PageCache: %u cached (%u hits / %u misses)\n",
            static_cast<unsigned>(g_page_cache.cached_pages()),
            static_cast<unsigned>(g_page_cache.hit_count()),
            static_cast<unsigned>(g_page_cache.miss_count()));

    // Delta vs the previous dump so the periodic stats thread's log shows a PF
    // rate, not just a monotonic total.  static: dump_memory_stats has no
    // concurrent callers in practice (panic once + the single stats thread).
    static uint64_t last_pf = 0;
    kprintf("[MEM] #PF:       %u total (+%u since last dump)\n",
            static_cast<unsigned>(pf_total),
            static_cast<unsigned>(pf_total - last_pf));
    last_pf = pf_total;

    // B2.5: cumulative ext2 read I/O (count + bytes + wall time) with delta --
    // attributes the residual gcc-compile stall to demand-PF I/O vs syscall/TCG.
    static uint64_t last_io_ns    = 0;
    static uint64_t last_io_reads = 0;
    const uint64_t io_ns          = cinux::fs::ext2_read_ns();
    const uint64_t io_reads       = cinux::fs::ext2_read_count();
    const uint64_t io_bytes       = cinux::fs::ext2_read_bytes();
    kprintf("[MEM] I/O:        %u reads (+%u), %u KiB, %llu ms (+%llu ms)\n",
            static_cast<unsigned>(io_reads),
            static_cast<unsigned>(io_reads - last_io_reads),
            static_cast<unsigned>(io_bytes / 1024),
            static_cast<unsigned long long>(io_ns / 1000000),
            static_cast<unsigned long long>((io_ns - last_io_ns) / 1000000));
    last_io_ns    = io_ns;
    last_io_reads = io_reads;
}

}  // namespace cinux::mm
