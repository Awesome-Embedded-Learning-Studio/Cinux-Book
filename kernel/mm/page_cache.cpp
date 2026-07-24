/**
 * @file kernel/mm/page_cache.cpp
 * @brief File-backed page cache implementation (F2-M4 batch 1)
 *
 * Namespace: cinux::mm
 */

#include <utility>

#include "kernel/mm/page_cache.hpp"

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/pmm.hpp"

namespace cinux::mm {

PageCache g_page_cache;

void PageCache::init(size_t max_pages) {
    // buckets_ is zero-initialised by the member initialiser; the cache is a
    // global, so this just records the advisory ceiling and clears the stats.
    max_pages_   = max_pages;
    total_pages_ = 0;
    hits_        = 0;
    misses_      = 0;
}

uint64_t PageCache::hash_key(cinux::fs::Inode* inode, uint64_t offset) {
    // Fold the inode NUMBER (stable) and page-aligned offset into a bucket.
    // Keying by inode number, not pointer: the slab allocator reuses freed
    // Inode memory, so a pointer key would alias a new file onto a stale page.
    return (inode->ino ^ (offset >> 12)) % kHashBuckets;
}

CachedPage* PageCache::lookup_locked(cinux::fs::Inode* inode, uint64_t offset) {
    CachedPage* p = buckets_[hash_key(inode, offset)];
    while (p != nullptr) {
        if (p->ino == inode->ino && p->offset == offset) {
            return p;
        }
        p = p->hash_next;
    }
    return nullptr;
}

CachedPage* PageCache::lookup(cinux::fs::Inode* inode, uint64_t offset) {
    // Lock-free read: safe because the cache is mutated only under IF=0 (page
    // fault) or in tests -- never from a reentrant IRQ (see header comment).
    return lookup_locked(inode, offset);
}

bool PageCache::contains_phys(uint64_t phys) const {
    // Lock-free: cached pages are never freed (no eviction yet), so bucket
    // chains only grow at the head -- a scan sees a stable snapshot. Used by
    // the always-on free check (pmm.cpp) to flag a free of a still-cached page.
    for (size_t b = 0; b < kHashBuckets; b++) {
        for (CachedPage* p = buckets_[b]; p != nullptr; p = p->hash_next) {
            if (p->phys == phys) {
                return true;
            }
        }
    }
    return false;
}

void PageCache::insert_locked(CachedPage* page) {
    const uint64_t b = hash_key(page->inode, page->offset);
    page->hash_prev  = nullptr;
    page->hash_next  = buckets_[b];
    if (buckets_[b] != nullptr) {
        buckets_[b]->hash_prev = page;
    }
    buckets_[b] = page;
}

cinux::lib::ErrorOr<CachedPage*> PageCache::get_page(cinux::fs::Inode* inode, uint64_t offset) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    // 1. Fast path: already cached -> bump ref and return (under lock).
    {
        auto g = lock_.irq_guard();
        if (CachedPage* hit = lookup_locked(inode, offset)) {
            hit->ref_count++;
            hits_++;
            return hit;
        }
        misses_++;
    }

    // 2. Miss: allocate a page WITH its cache-ownership ref and read the file
    //    content into it.  alloc_page sets refcount=1 (pte_count=0) -- that
    //    single refcount IS the page cache's ownership, kept alive for the
    //    CachedPage (dropped on evict/free).  Because the cache holds it,
    //    process teardown (free_subtree) and munmap -- which only drop the PTE
    //    refs they added -- can never reach 0 and free a page the cache still
    //    points at (the lto_plugin corruption root cause, fix f06ea6b).
    //    This is the slow I/O path and runs OUTSIDE the cache lock -- never do
    //    I/O while holding the lock, or a reentrant page fault at IF=0 would
    //    deadlock (F2-M4 GOTCHA).  On any failure below the local `own` drops
    //    its ref (pte_count 1 -> 0 -> free) itself; nothing frees raw.
    CachePhysRef own = CachePhysRef::alloc();
    if (!own.valid()) {
        return cinux::lib::Error::OutOfMemory;
    }
    const uint64_t phys = own.phys();
    const uint64_t virt = phys + cinux::arch::DIRECT_MAP_BASE;

    // Start from a clean zero page so any short read (file tail) is zero-filled.
    memset(reinterpret_cast<void*>(virt), 0, cinux::arch::PAGE_SIZE);

    auto read_res =
        inode->ops->read(inode, offset, reinterpret_cast<void*>(virt), cinux::arch::PAGE_SIZE);
    if (!read_res.ok()) {
        return read_res.error();  // ~own drops the ref and frees phys
    }
    // read_res.value() bytes were filled; the remainder stays zero (EOF pad).

    // 3. Wrap the page and insert under a short locked section.  Re-check for a
    //    concurrent insert of the same key and, on collision, drop our page.
    auto* page = new CachedPage();
    if (page == nullptr) {
        return cinux::lib::Error::OutOfMemory;  // ~own frees phys
    }
    page->ino       = inode->ino;
    page->inode     = inode;
    page->offset    = offset;
    page->phys      = phys;
    page->virt      = virt;
    page->valid     = true;
    page->ref_count = 1;
    page->own       = std::move(own);  // CachedPage takes ownership

    {
        auto g = lock_.irq_guard();
        if (CachedPage* race = lookup_locked(inode, offset)) {
            race->ref_count++;
            hits_++;
            delete page;  // ~CachePhysRef frees phys (own was moved into it)
            return race;
        }
        insert_locked(page);
        total_pages_++;
    }
    return page;
}

void PageCache::release(CachedPage* page) {
    if (page == nullptr) {
        return;
    }
    auto g = lock_.irq_guard();
    if (page->ref_count > 0) {
        page->ref_count--;
    }
}

void PageCache::invalidate_range(cinux::fs::Inode* inode, uint64_t file_off, uint64_t count) {
    if (inode == nullptr || inode->ops == nullptr || count == 0) {
        return;
    }
    const uint64_t ps    = cinux::arch::PAGE_SIZE;
    const uint64_t mask  = ps - 1;
    const uint64_t start = file_off & ~mask;
    const uint64_t last  = (file_off + count - 1) & ~mask;
    for (uint64_t off = start;; off += ps) {
        CachedPage* p = nullptr;
        {
            // Lookup under the lock; the disk re-read below runs outside it
            // (no I/O under the cache lock -- F2-M4 GOTCHA).
            auto g = lock_.irq_guard();
            p      = lookup_locked(inode, off);
        }
        if (p != nullptr) {
            // Refresh the page in place: same physical page, so PTEs that
            // currently map it stay valid and just observe the new bytes.
            // memset first so a short read (EOF tail) stays zero-padded,
            // mirroring get_page()'s initial fill.
            void* v = reinterpret_cast<void*>(p->virt);
            memset(v, 0, ps);
            static_cast<void>(inode->ops->read(inode, off, v, ps));
        }
        if (off == last) {
            break;
        }
    }
}

cinux::lib::ErrorOr<int64_t> PageCache::read_bytes(cinux::fs::Inode* inode, uint64_t file_off,
                                                   void* buf, uint64_t count) {
    if (inode == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    // EOF: nothing to deliver at or past the end of file.
    if (file_off >= inode->size) {
        return 0;
    }

    const uint64_t end = ((file_off + count) > inode->size) ? inode->size : (file_off + count);
    const uint64_t page_mask = static_cast<uint64_t>(cinux::arch::PAGE_SIZE) - 1;
    auto*          dst       = static_cast<uint8_t*>(buf);
    uint64_t       done      = 0;
    uint64_t       cur       = file_off;

    while (cur < end) {
        const uint64_t page_base = cur & ~page_mask;
        const uint64_t in_page   = cur - page_base;
        const uint64_t chunk     = (cinux::arch::PAGE_SIZE - in_page < end - cur)
                                       ? (cinux::arch::PAGE_SIZE - in_page)
                                       : (end - cur);

        // Pull the page through the cache (fill on miss, hit on repeat).  No I/O
        // happens under the cache lock (see get_page), so this is safe from the
        // syscall path just as it is from the page-fault path.
        auto gp = get_page(inode, page_base);
        if (!gp.ok()) {
            // Return whatever was already read; only surface the error if we
            // delivered nothing (matches read()'s partial-read contract).
            if (done > 0) {
                break;
            }
            return gp.error();
        }
        CachedPage* cp = gp.value();
        memcpy(dst + done, reinterpret_cast<void*>(cp->virt + in_page), chunk);
        release(cp);

        cur += chunk;
        done += chunk;
    }

    return static_cast<int64_t>(done);
}

size_t PageCache::cached_pages() const {
    return total_pages_;
}

size_t PageCache::hit_count() const {
    return hits_;
}

size_t PageCache::miss_count() const {
    return misses_;
}

}  // namespace cinux::mm
