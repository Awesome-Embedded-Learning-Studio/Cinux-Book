/**
 * @file kernel/mm/address_space.cpp
 * @brief Per-process virtual address space management implementation
 */

#include "kernel/mm/address_space.hpp"

#include <stddef.h>
#include <stdint.h>

#include <utility>

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

// ============================================================
// Static member initialisation
// ============================================================

uint64_t AddressSpace::kernel_pml4_ = 0;

// ============================================================
// Constants
// ============================================================

using namespace cinux::arch;

// PML4 entry indices that belong to user space (lower half)
constexpr uint32_t USER_PML4_START = 0;
constexpr uint32_t USER_PML4_END   = 256;

// Recursion levels for subtree freeing:
//   PML4 -> PDPT (level 3) -> PD (level 2) -> PT (level 1)
constexpr int LEVEL_PDPT = 3;
constexpr int LEVEL_PD   = 2;
constexpr int LEVEL_PT   = 1;

// ============================================================
// Internal helpers
// ============================================================

// ============================================================
// Static initialisation
// ============================================================

void AddressSpace::init_kernel() {
    kernel_pml4_ = cinux::arch::read_cr3();
    cinux::lib::kprintf("[AS] Kernel PML4 saved at phys %p\n",
                        reinterpret_cast<void*>(kernel_pml4_));
}

// ============================================================
// Construction
// ============================================================

AddressSpace::AddressSpace() {
    // Step 1: Allocate a fresh PML4 page
    pml4_phys_ = g_pmm.alloc_page();
    if (pml4_phys_ == 0) {
        cinux::lib::kprintf("[AS] FATAL: failed to allocate PML4 page\n");
        return;
    }

    // Step 2: Zero the entire PML4
    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        pml4[i].raw = 0;
    }

    auto* kern_pml4 = phys_to_virt(kernel_pml4_);
    for (uint32_t i = USER_PML4_END; i < PT_ENTRIES; i++) {
        pml4[i].raw = kern_pml4[i].raw;
    }
}

// ============================================================
// Destruction
// ============================================================

AddressSpace::~AddressSpace() {
    // Nothing to free if the PML4 was never allocated (or was moved out)
    if (pml4_phys_ == 0) {
        return;
    }

    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++) {
        if (pml4[i].is_present()) {
            free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);
        }
    }

    // Step 2: Free the PML4 page itself
    g_pmm.free_page(pml4_phys_);
    pml4_phys_ = 0;
}

// ============================================================
// Move operations
// ============================================================

AddressSpace::AddressSpace(AddressSpace&& other) noexcept
    : pml4_phys_(other.pml4_phys_), vma_store_(std::move(other.vma_store_)) {
    other.pml4_phys_ = 0;
}

AddressSpace& AddressSpace::operator=(AddressSpace&& other) noexcept {
    if (this != &other) {
        // Free our current resources
        if (pml4_phys_ != 0) {
            auto* pml4 = phys_to_virt(pml4_phys_);
            for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++) {
                if (pml4[i].is_present()) {
                    free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);
                }
            }
            g_pmm.free_page(pml4_phys_);
        }

        // Take ownership of the other's PML4 and VMA store
        pml4_phys_       = other.pml4_phys_;
        other.pml4_phys_ = 0;
        vma_store_       = std::move(other.vma_store_);
    }
    return *this;
}

// ============================================================
// Page table operations
// ============================================================

bool AddressSpace::map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return g_vmm.map(virt, phys, flags, &pml4_phys_);
}

void AddressSpace::unmap(uint64_t virt) {
    g_vmm.unmap(virt, &pml4_phys_);
}

uint64_t AddressSpace::translate(uint64_t virt) {
    return g_vmm.translate(virt, &pml4_phys_);
}

void AddressSpace::activate() {
    cinux::arch::write_cr3(pml4_phys_);
}

// ============================================================
// Accessors
// ============================================================

uint64_t AddressSpace::pml4_phys() const {
    return pml4_phys_;
}

uint64_t AddressSpace::kernel_pml4() {
    return kernel_pml4_;
}

// ============================================================
// Internal helpers
// ============================================================

void AddressSpace::free_subtree(uint64_t table_phys, int level) {
    auto* table = phys_to_virt(table_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!table[i].is_present()) {
            continue;
        }

        // DEBT-009: a huge entry (1 GB at PDPT, 2 MB at PD) maps a data page,
        // not a child page table -- descending would parse the huge-page body as
        // PT entries and free garbage.  Huge-page free (buddy order) isn't wired
        // yet (no user huge mappings); warn and skip.  Encountering this
        // means a future huge-mapping milestone forgot to update this path.
        if (table[i].huge) {
            cinux::lib::kprintf(
                "[MM] free_subtree: huge entry @ level %d phys=0x%lx "
                "skipped (huge free unimplemented)\n",
                level, static_cast<unsigned long>(table[i].phys_addr()));
            continue;
        }

        if (level == LEVEL_PT) {
            uint64_t data_phys = table[i].phys_addr();
            if (g_pmm.mapcount_dec_and_test(data_phys)) {
                g_pmm.free_page(data_phys);
            }
            table[i].raw = 0;
            continue;
        }

        free_subtree(table[i].phys_addr(), level - 1);
        g_pmm.free_page(table[i].phys_addr());
        table[i].raw = 0;
    }
}

}  // namespace cinux::mm
