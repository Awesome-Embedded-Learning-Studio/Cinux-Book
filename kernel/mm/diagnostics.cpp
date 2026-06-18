/**
 * @file kernel/mm/diagnostics.cpp
 * @brief Memory-state summary implementation
 *
 * Aggregates the public stats of each MM subsystem into a compact dump.
 * kprintf only supports %u (unsigned int), so every 64-bit counter is cast to
 * unsigned -- safe here because kernel page/counts are far below 2^32.
 */

#include "kernel/mm/diagnostics.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/slab.hpp"

namespace cinux::mm {

using cinux::lib::kprintf;

void dump_memory_stats() {
    const uint64_t free_pages  = g_pmm.free_page_count();
    const uint64_t total_pages = g_pmm.total_page_count();

    kprintf("[MEM] PMM:       %u / %u pages free (%u KiB free of %u KiB)\n",
            (unsigned)free_pages, (unsigned)total_pages, (unsigned)(free_pages * 4),
            (unsigned)(total_pages * 4));
    kprintf("[MEM] Slab:      %u slab pages mapped\n",
            (unsigned)g_slab.total_slab_pages());
    kprintf("[MEM] PageCache: %u cached (%u hits / %u misses)\n",
            (unsigned)g_page_cache.cached_pages(), (unsigned)g_page_cache.hit_count(),
            (unsigned)g_page_cache.miss_count());
}

}  // namespace cinux::mm
