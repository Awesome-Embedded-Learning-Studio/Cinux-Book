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
 * Today (batch 1, transitional) PhysRef routes through the public pte_count_*
 * PMM API, which still conflates ownership + PTE in one counter (same
 * semantics as the old mapcount).  Batch 3 splits the storage so pte_count_dec
 * stops freeing; PhysRef then switches to the private refcount_* API.
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
            g_pmm.pte_count_inc(phys_);  // transitional: shares the unified counter
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
            if (g_pmm.pte_count_dec_and_test(phys_)) {  // transitional
                g_pmm.free_page(phys_);
            }
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
