/**
 * @file kernel/mm/dedicated_caches.cpp
 * @brief Dedicated slab caches for hot kernel object types (F2-M7b batch 3)
 *
 * One cache per heap-allocated hot type, created after g_slab.init.  Each type
 * routes its class-specific operator new/delete to its cache (see process.hpp /
 * vma.hpp / page_cache.hpp), so callers keep using plain `new` / `delete`.
 *
 * Inode is deliberately NOT here: ext2 caches VFS inodes in a fixed
 * inode_cache_[] array (kernel/fs/ext2.hpp), so they are never heap-allocated
 * and need no slab cache.
 *
 * Namespace: cinux::mm
 */

#include "kernel/mm/slab.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/page_cache.hpp"  // CachedPage
#include "kernel/mm/vma.hpp"         // VMA
#include "kernel/proc/process.hpp"   // Task

namespace cinux::mm {

SlabCache* g_task_cache        = nullptr;
SlabCache* g_vma_cache         = nullptr;
SlabCache* g_cached_page_cache = nullptr;

void init_dedicated_caches() {
    g_task_cache        = g_slab.create_cache("task", sizeof(cinux::proc::Task));
    g_vma_cache         = g_slab.create_cache("vma", sizeof(cinux::mm::VMA));
    g_cached_page_cache = g_slab.create_cache("cached_page", sizeof(cinux::mm::CachedPage));

    cinux::lib::kprintf("[SLAB] dedicated caches: task=%uB vma=%uB cached_page=%uB\n",
                        static_cast<unsigned>(sizeof(cinux::proc::Task)),
                        static_cast<unsigned>(sizeof(cinux::mm::VMA)),
                        static_cast<unsigned>(sizeof(cinux::mm::CachedPage)));
}

}  // namespace cinux::mm
