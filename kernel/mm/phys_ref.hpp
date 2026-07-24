/**
 * @file kernel/mm/phys_ref.hpp
 * @brief PhysRef<Tag>: RAII ownership handle for a physical page
 *
 * Move-only handle that owns one "refcount" on a physical page.  Destructor
 * drops the ref; on the last ref the page returns to the PMM.  Distinct from
 * pte_count (user-PTE tracking): PhysRef is the *ownership* dimension -- the
 * page-cache / VMA / anon holder keeps the page alive regardless of how many
 * PTEs map it.  Mirrors Linux's split between page->_refcount (ownership) and
 * _mapcount (user PTEs).
 *
 * Tag differentiates owner kind at the type level: a CachePhysRef cannot be
 * assigned to an AnonPhysRef (compile-time guard against cross-owner frees).
 *
 * Batch 3 split: PhysRef routes through the refcount_* PMM API (ownership
 * dimension); pte_count_* is now a separate, free-neutral mapping counter.
 * pte_count_dec_and_test (used by teardown/unmap callers) bundles a pte_count
 * drop with a conditional refcount drop, so a page still owned by a CachePhysRef
 * survives teardown even with pte_count == 0 (the lto_plugin corruption root
 * cause f06ea6b prevented without a phantom pte_count+1).
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stdint.h>

#include "kernel/mm/pmm.hpp"

namespace cinux::mm {

template <typename Tag>
class PhysRef {
public:
    /// Empty handle (phys_ == 0).  Lets owners (e.g. CachedPage) default-
    /// construct then move-assign a real page in.  Destructor is a no-op on
    /// an empty handle.
    PhysRef() noexcept = default;

    /// Allocate a fresh page; refcount starts at 1 (set by PMM::alloc_page).
    static PhysRef alloc() { return PhysRef(g_pmm.alloc_page()); }

    /// Drop the ownership ref; on the last ref the page goes back to the PMM.
    ~PhysRef() { drop_(); }

    PhysRef(PhysRef&& other) noexcept : phys_(other.phys_) { other.phys_ = 0; }
    PhysRef& operator=(PhysRef&& other) noexcept {
        if (this != &other) {
            drop_();
            phys_       = other.phys_;
            other.phys_ = 0;
        }
        return *this;
    }
    PhysRef(const PhysRef&)            = delete;  // no copy -> no double-free
    PhysRef& operator=(const PhysRef&) = delete;

    /// Take another ownership ref on the same page (for VMA / fork / cache).
    PhysRef share() const {
        if (phys_ != 0) {
            g_pmm.refcount_inc(phys_);  // batch 3: ownership dimension (was pte_count)
        }
        return PhysRef(phys_);
    }

    uint64_t phys() const { return phys_; }  ///< read-only escape (PTE / DMA)
    bool     valid() const { return phys_ != 0; }

    /// Relinquish ownership WITHOUT dropping the ref; caller becomes the raw
    /// owner.  Rare escape hatch (e.g. handing phys to a C/asm shim).
    uint64_t release() {
        uint64_t p = phys_;
        phys_      = 0;
        return p;
    }

private:
    explicit PhysRef(uint64_t p) : phys_(p) {}
    void drop_() {
        if (phys_ != 0) {
            // batch 3: drops one ownership ref.  On the last ref the page goes
            // back to the buddy inside refcount_dec_and_test (was a two-step
            // pte_count_dec_and_test + free_page under the unified counter).
            g_pmm.refcount_dec_and_test(phys_);
            phys_ = 0;
        }
    }
    uint64_t phys_{0};
};

struct CachePageTag {};     ///< page-cache-owned (file-backed) page
struct AnonPageTag {};      ///< anonymous (process-private / CoW) page
struct PageTableTag {};     ///< page-table page (PML4/PDPT/PD/PT)

using CachePhysRef     = PhysRef<CachePageTag>;
using AnonPhysRef      = PhysRef<AnonPageTag>;
using PageTablePhysRef = PhysRef<PageTableTag>;

}  // namespace cinux::mm
