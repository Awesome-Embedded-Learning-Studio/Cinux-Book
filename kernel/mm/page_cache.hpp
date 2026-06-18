/**
 * @file kernel/mm/page_cache.hpp
 * @brief File-backed page cache (F2-M4 batch 1)
 *
 * Caches file contents as physical pages keyed by (Inode*, page-aligned file
 * offset).  Serves two callers:
 *   - The page-fault handler (batch 2): on a file-backed VMA miss it fills a
 *     page with the file's content and maps it, so mmap'd files read real
 *     bytes instead of the zeroes the anonymous demand-pager hands out today.
 *   - Tests (this batch): unit-test the cache in isolation with a fake inode.
 *
 * Design notes (see document/ai/PLAN.md F2-M4):
 *   - Cached pages live in the direct map: virt = phys + DIRECT_MAP_BASE,
 *     so no temporary mapping is needed and pages are never unmapped (GOTCHA #7,
 *     same model as the F1-M3 DmaPool).
 *   - lookup() is lock-free: safe because the cache is mutated only from the
 *     page-fault handler (IF=0, single CPU) or test context -- never from a
 *     reentrant IRQ.  get_page() reads file content OUTSIDE the cache lock and
 *     inserts under a short irq-guarded critical section, so no I/O happens
 *     while the lock is held (a reentrant fault at IF=0 would otherwise
 *     deadlock -- F2-M4 GOTCHA).
 *
 * MVP scope: read path + reference counting only.  Dirty-page writeback,
 * MAP_SHARED write coherence, LRU eviction, and cross-process sharing are
 * deferred to later milestones.
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/proc/sync.hpp"
#include <new>

#include "kernel/mm/slab.hpp"

namespace cinux::fs {
struct Inode;
}

namespace cinux::mm {

/// A single cached file page.  Owned by the PageCache hash table.
struct CachedPage {
    // F2-M7b: heap CachedPage objects are served by the dedicated cache.
    static void*       operator new(size_t) {
        return cinux::mm::cache_alloc(cinux::mm::g_cached_page_cache);
    }
    static void*       operator new(size_t, std::align_val_t) {
        return cinux::mm::cache_alloc(cinux::mm::g_cached_page_cache);
    }
    static void        operator delete(void* p) {
        cinux::mm::cache_free(cinux::mm::g_cached_page_cache, p);
    }
    static void        operator delete(void* p, std::align_val_t) {
        cinux::mm::cache_free(cinux::mm::g_cached_page_cache, p);
    }

    /// Backing inode NUMBER -- the stable lookup key (part 1).  The inode
    /// POINTER is deliberately NOT the key: the slab allocator reuses freed
    /// Inode memory, so keying by pointer would alias a brand-new file onto a
    /// stale cached page and serve wrong content (F2-M7b).
    uint64_t          ino{0};
    cinux::fs::Inode* inode{nullptr};  ///< Backing inode (transient; not a key)
    uint64_t          offset{0};       ///< File offset, page-aligned (key, part 2)
    uint64_t          phys{0};         ///< Physical page address
    uint64_t          virt{0};         ///< Kernel vaddr = phys + DIRECT_MAP_BASE (direct map)
    uint32_t          ref_count{0};    ///< Outstanding users (no eviction yet)
    bool              valid{false};    ///< File content has been loaded

    CachedPage* hash_next{nullptr};  ///< Hash-bucket chain
    CachedPage* hash_prev{nullptr};
};

/**
 * @brief File-backed page cache backed by a fixed-size hash table.
 *
 * 256 buckets chained with intrusive doubly-linked lists, keyed by (inode
 * pointer, page-aligned offset).  Read path only for now (no writeback).
 */
class PageCache {
public:
    /// Record the (currently advisory) page ceiling and reset stats.  Eviction
    /// is not yet implemented; @p max_pages is stored for the future LRU policy.
    void init(size_t max_pages);

    /// Lock-free lookup.  Returns the cached page for (inode, offset) or
    /// nullptr.  Safe from the page-fault handler (IF=0) per the file comment.
    CachedPage* lookup(cinux::fs::Inode* inode, uint64_t offset);

    /// Obtain a page: a hit bumps the ref count and returns; a miss allocates a
    /// physical page, reads file content via inode->ops->read (zero-padding any
    /// EOF tail), and inserts.  Returns OutOfMemory on allocation failure or the
    /// propagated read error.
    cinux::lib::ErrorOr<CachedPage*> get_page(cinux::fs::Inode* inode, uint64_t offset);

    /// Read @p count bytes of file @p inode starting at @p file_off into @p buf,
    /// serving each page-aligned slice from the cache (filling on miss via the
    /// inode's disk read).  This is the cache-aware read path used by sys_read
    /// for page-cacheable (disk-backed) inodes: it reuses get_page so that
    /// repeated reads and demand paging share one copy of each page.  EOF is
    /// honoured via inode->size; a short page tail is zero-padded by get_page.
    /// Returns the number of bytes read (0 at/past EOF).
    cinux::lib::ErrorOr<int64_t> read_bytes(cinux::fs::Inode* inode, uint64_t file_off, void* buf,
                                            uint64_t count);

    /// Decrement the reference count of @p page (floors at 0).  No eviction.
    void release(CachedPage* page);

    size_t cached_pages() const;  ///< Pages currently cached
    size_t hit_count() const;     ///< get_page hits since init
    size_t miss_count() const;    ///< get_page misses since init

private:
    static constexpr size_t kHashBuckets = 256;

    CachedPage*           buckets_[kHashBuckets]{};
    cinux::proc::Spinlock lock_;
    size_t                max_pages_{0};
    size_t                total_pages_{0};
    size_t                hits_{0};
    size_t                misses_{0};

    static uint64_t hash_key(cinux::fs::Inode* inode, uint64_t offset);
    CachedPage*     lookup_locked(cinux::fs::Inode* inode, uint64_t offset);
    void            insert_locked(CachedPage* page);
};

/// Global page-cache instance.
extern PageCache g_page_cache;

}  // namespace cinux::mm
