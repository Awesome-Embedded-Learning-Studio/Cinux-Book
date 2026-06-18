/**
 * @file kernel/mm/slab.cpp
 * @brief SLAB small-object allocator implementation (F2-M7b batch 1)
 */

#include "kernel/mm/slab.hpp"

#include <stddef.h>
#include <stdint.h>

#include <cinux/numeric.hpp>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

// ============================================================
// Global instance
// ============================================================

SlabAllocator g_slab;

// ============================================================
// Helpers
// ============================================================

namespace {

using cinux::lib::align_up;

constexpr uint64_t kPageFlags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE;

/// Sentinel written to a freed object's second word (the first word holds the
/// intrusive freelist link).  Fresh slab pages and freshly allocated objects
/// are zeroed, so encountering this value on free is a repeated free.
constexpr uint64_t kSlabPoison = 0xFEEDC0DEFEEDC0DEULL;

/// Object alignment for every cache (general and dedicated).  16 satisfies the
/// default operator-new alignment; a fixed value lets dedicated caches use
/// arbitrary (non-power-of-two) object sizes while keeping every object aligned.
constexpr size_t kObjAlign = 16;

void memzero(void* start, size_t len) {
    auto* p = static_cast<uint8_t*>(start);
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

}  // namespace

// ============================================================
// SlabAllocator::init
// ============================================================

void SlabAllocator::init(uint64_t virt_base, uint64_t region_size) {
    slab_base_  = virt_base;
    slab_brk_   = virt_base;
    slab_end_   = virt_base + region_size;
    slab_pages_ = 0;

    for (int i = 0; i < kNumCaches; i++) {
        SlabCache& c   = caches_[i];
        c.obj_size      = kSlabMinObj << i;
        uint64_t hdr    = align_up(sizeof(SlabHeader), kObjAlign);
        c.objs_per_slab = static_cast<uint16_t>((cinux::arch::PAGE_SIZE - hdr) / c.obj_size);
        c.name          = "general";
        c.partial       = nullptr;
        c.full          = nullptr;
        c.slab_count    = 0;
        c.ctor          = nullptr;
        c.dtor          = nullptr;
    }

    cinux::lib::kprintf("[SLAB] Initialised: %d general caches (%u..%u B), window %u MB\n",
                        kNumCaches, static_cast<unsigned>(kSlabMinObj),
                        static_cast<unsigned>(kSlabMaxObj),
                        static_cast<unsigned>(region_size / (1024 * 1024)));
}

// ============================================================
// size_to_index: smallest general cache that holds @p size, or -1
// ============================================================

int SlabAllocator::size_to_index(size_t size) {
    if (size <= kSlabMinObj) {
        return 0;
    }
    if (size > kSlabMaxObj) {
        return -1;
    }
    size_t s   = kSlabMinObj;
    int    idx = 0;
    while (s < size) {
        s <<= 1;
        idx++;
    }
    return idx < kNumCaches ? idx : -1;
}

// ============================================================
// Doubly-linked list ops over SlabHeader (next/prev)
// ============================================================

void SlabAllocator::list_push_front(SlabHeader** head, SlabHeader* s) {
    s->prev = nullptr;
    s->next = *head;
    if (*head != nullptr) {
        (*head)->prev = s;
    }
    *head = s;
}

void SlabAllocator::list_remove(SlabHeader** head, SlabHeader* s) {
    if (s->prev != nullptr) {
        s->prev->next = s->next;
    } else {
        *head = s->next;
    }
    if (s->next != nullptr) {
        s->next->prev = s->prev;
    }
    s->next = nullptr;
    s->prev = nullptr;
}

// ============================================================
// grow_cache: map one fresh slab page and build its free list
// ============================================================

SlabHeader* SlabAllocator::grow_cache(SlabCache& cache) {
    if (slab_brk_ + cinux::arch::PAGE_SIZE > slab_end_) {
        cinux::lib::kprintf("[SLAB] OOM: virtual window exhausted\n");
        return nullptr;
    }
    uint64_t virt = slab_brk_;
    uint64_t phys = g_pmm.alloc_page();
    if (phys == 0) {
        return nullptr;
    }
    if (!g_vmm.map(virt, phys, kPageFlags)) {
        g_pmm.free_page(phys);
        return nullptr;
    }
    // Zero the whole page: a clean baseline for the double-free poison guard,
    // and no stale physical data leaks to a freshly allocated object.
    memzero(reinterpret_cast<void*>(virt), cinux::arch::PAGE_SIZE);

    slab_brk_ += cinux::arch::PAGE_SIZE;
    slab_pages_++;

    auto* s  = reinterpret_cast<SlabHeader*>(virt);
    s->cache = &cache;
    s->inuse = 0;
    s->total = cache.objs_per_slab;
    s->next  = nullptr;
    s->prev  = nullptr;

    // Build the intrusive free list: object[i].link = &object[i+1].
    uint64_t hdr_area = align_up(sizeof(SlabHeader), kObjAlign);
    uint64_t base     = virt + hdr_area;
    s->freelist       = reinterpret_cast<void*>(base);
    for (uint16_t i = 0; i < cache.objs_per_slab; i++) {
        void** slot = reinterpret_cast<void**>(base + i * cache.obj_size);
        *slot       = (i + 1 < cache.objs_per_slab)
                        ? reinterpret_cast<void*>(base + (i + 1) * cache.obj_size)
                        : nullptr;
    }

    list_push_front(&cache.partial, s);
    cache.slab_count++;
    return s;
}

// ============================================================
// alloc_locked (caller holds the spinlock / interrupts disabled)
// ============================================================

void* SlabAllocator::cache_alloc_locked(SlabCache& c) {
    SlabHeader* s = c.partial;
    if (s == nullptr) {
        s = grow_cache(c);
        if (s == nullptr) {
            return nullptr;
        }
    }

    void* obj = s->freelist;
    // Pop the head: the object's first word holds the next free pointer.
    s->freelist = *reinterpret_cast<void**>(obj);
    s->inuse++;

    if (s->inuse == s->total) {
        list_remove(&c.partial, s);
        list_push_front(&c.full, s);
    }

    memzero(obj, c.obj_size);  // zero + clears the link word + poison baseline
    return obj;
}

void* SlabAllocator::alloc_locked(size_t size) {
    if (size == 0) {
        return nullptr;
    }
    int idx = size_to_index(size);
    if (idx < 0) {
        return nullptr;  // not slab-eligible
    }
    return cache_alloc_locked(caches_[idx]);
}

// ============================================================
// free_locked (caller holds the spinlock / interrupts disabled)
// ============================================================

void SlabAllocator::free_locked(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    // Strong guard: only pointers inside the slab window are slab objects.
    if (p < slab_base_ || p >= slab_brk_) {
        return;
    }

    // The owning slab page is the page-aligned base of the pointer.
    auto* s = reinterpret_cast<SlabHeader*>(p & ~static_cast<uintptr_t>(cinux::arch::PAGE_SIZE - 1));
    if (s->cache == nullptr) {
        return;  // not an initialised slab page
    }
    SlabCache* c = s->cache;

    // Double-free guard (O(1)): a freed object carries the poison sentinel in
    // its second word (the first word holds the freelist link).  Fresh slab
    // pages and freshly allocated objects are zeroed, so a poison hit here is a
    // repeated free of the same slot.
    uint64_t* words = reinterpret_cast<uint64_t*>(ptr);
    if (words[1] == kSlabPoison) {
        cinux::lib::kprintf("[SLAB] double-free at 0x%p\n", ptr);
        return;
    }

    bool was_full = (s->inuse == s->total);
    // Push back onto the intrusive free list and mark the slot poisoned.
    words[0]    = reinterpret_cast<uintptr_t>(s->freelist);
    words[1]    = kSlabPoison;
    s->freelist = ptr;
    s->inuse--;

    if (was_full) {
        list_remove(&c->full, s);
        list_push_front(&c->partial, s);
    }
}

// ============================================================
// Public alloc / free (IRQ-safe spinlock)
// ============================================================

void* SlabAllocator::alloc(size_t size) {
    auto g = lock_.irq_guard();
    (void)g;
    return alloc_locked(size);
}

void SlabAllocator::free(void* ptr) {
    auto g = lock_.irq_guard();
    (void)g;
    free_locked(ptr);
}

// ============================================================
// Dedicated caches (F2-M7b batch 3)
// ============================================================

SlabCache* SlabAllocator::create_cache(const char* name, size_t obj_size,
                                       void (*ctor)(void*), void (*dtor)(void*)) {
    // Round the object size up to the object alignment so every object in a
    // slab stays 16-aligned (lets dedicated caches hold arbitrary sizes).
    if (obj_size < kSlabMinObj) {
        obj_size = kSlabMinObj;
    }
    obj_size = align_up(obj_size, kObjAlign);

    auto* c = static_cast<SlabCache*>(kmalloc(sizeof(SlabCache)));
    if (c == nullptr) {
        return nullptr;
    }
    uint64_t hdr = align_up(sizeof(SlabHeader), kObjAlign);
    c->obj_size      = obj_size;
    c->objs_per_slab = static_cast<uint16_t>((cinux::arch::PAGE_SIZE - hdr) / obj_size);
    c->name          = name;
    c->partial       = nullptr;
    c->full          = nullptr;
    c->slab_count    = 0;
    c->ctor          = ctor;
    c->dtor          = dtor;
    return c;
}

void* SlabAllocator::cache_alloc(SlabCache* cache) {
    if (cache == nullptr) {
        return nullptr;
    }
    auto g = lock_.irq_guard();
    (void)g;
    void* obj = cache_alloc_locked(*cache);
    if (obj != nullptr && cache->ctor != nullptr) {
        cache->ctor(obj);
    }
    return obj;
}

void SlabAllocator::cache_free(SlabCache* cache, void* obj) {
    if (cache == nullptr || obj == nullptr) {
        return;
    }
    if (cache->dtor != nullptr) {
        cache->dtor(obj);
    }
    auto g = lock_.irq_guard();
    (void)g;
    free_locked(obj);  // the slab page header records the owning cache
}

// ============================================================
// kmalloc / kfree -- universal allocator (slab for small, buddy+direct-map
// for large).  See slab.hpp.
// ============================================================

void* kmalloc(size_t size, size_t align) {
    if (size == 0) {
        return nullptr;
    }

    // Effective class must honour both the requested size and alignment.
    size_t eff = size > align ? size : align;
    if (eff <= kSlabMaxObj) {
        return g_slab.alloc(eff);
    }

    // Large allocation: whole buddy pages exposed through the direct map.
    uint64_t npages =
        (static_cast<uint64_t>(size) + cinux::arch::PAGE_SIZE - 1) / cinux::arch::PAGE_SIZE;
    if (align > cinux::arch::PAGE_SIZE) {
        uint64_t align_pages = align / cinux::arch::PAGE_SIZE;
        if (align_pages > npages) {
            npages = align_pages;
        }
    }
    uint64_t phys = g_pmm.alloc_pages(npages);
    if (phys == 0) {
        return nullptr;
    }
    void* virt = reinterpret_cast<void*>(phys + cinux::arch::DIRECT_MAP_BASE);
    memzero(virt, size);  // match the slab path: no stale-data leak
    return virt;
}

void kfree(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);

    // Slab window -> small-object cache.
    if (p >= cinux::arch::KMEM_SLAB_BASE &&
        p < cinux::arch::KMEM_SLAB_BASE + cinux::arch::KMEM_SLAB_SIZE) {
        g_slab.free(ptr);
        return;
    }

    // Direct-map window -> buddy pages (count immaterial; buddy has the order).
    if (p >= cinux::arch::DIRECT_MAP_BASE) {
        g_pmm.free_pages(p - cinux::arch::DIRECT_MAP_BASE, 0);
        return;
    }

    // Unknown pointer: ignore defensively.
}

}  // namespace cinux::mm
