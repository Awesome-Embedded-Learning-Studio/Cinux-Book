/**
 * @file kernel/arch/x86_64/paging.cpp
 * @brief Minimal paging implementation for the big kernel
 *
 * Manipulates the page tables at their known virtual addresses
 * (set up by the bootloader and extended by the mini kernel) to
 * map MMIO regions such as the framebuffer.
 */

#include "kernel/arch/x86_64/paging.hpp"

#include <stdint.h>

namespace cinux::arch {

namespace {

// Page directory entry flags: Present + Read/Write + Huge (2MB page)
constexpr uint64_t PD_HUGE_PAGE_FLAGS = 0x83;

// PDPT entry flags: Present + Read/Write + Page Size (1GB page)
constexpr uint64_t PDPT_1GB_PAGE_FLAGS = 0x83;

// Size of a 2MB huge page
constexpr uint64_t PAGE_2MB_SIZE = 0x200000;

// Size of a 1GB huge page
constexpr uint64_t PAGE_1GB_SIZE = 0x40000000ULL;

// Entries per page table level
constexpr uint32_t PT_ENTRIES = 512;

// Known virtual addresses of page tables (higher-half mapped)
constexpr uint64_t PD_VIRT_ADDR = 0xFFFFFFFF80003000ULL;
constexpr uint64_t PDPT_VIRT_ADDR = 0xFFFFFFFF80002000ULL;

// PDPT[0] points to PD -- do not overwrite
constexpr uint32_t PDPT_PD_ENTRY = 0;

bool has_1gb_pages() {
    uint32_t eax = 0x80000001;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    return (edx & (1u << 26)) != 0;
}

void reload_cr3() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

}  // anonymous namespace

void map_mmio(uint64_t phys, uint64_t size) {
    auto* pd = reinterpret_cast<volatile uint64_t*>(PD_VIRT_ADDR);
    auto* pdpt = reinterpret_cast<volatile uint64_t*>(PDPT_VIRT_ADDR);

    uint64_t end = phys + size;

    // Part 1: PD entries for range within first 1GB (2MB pages)
    uint64_t cur = phys & ~(PAGE_2MB_SIZE - 1);
    while (cur < end && cur < PAGE_1GB_SIZE) {
        uint32_t idx = static_cast<uint32_t>(cur / PAGE_2MB_SIZE);
        if (idx < PT_ENTRIES && pd[idx] == 0) {
            pd[idx] = cur | PD_HUGE_PAGE_FLAGS;
            __asm__ volatile("invlpg (%0)" : : "r"(cur));
        }
        cur += PAGE_2MB_SIZE;
    }

    // Part 2: PDPT entries for range >= 1GB (1GB pages)
    if (end > PAGE_1GB_SIZE && has_1gb_pages()) {
        uint64_t cur1g = phys & ~(PAGE_1GB_SIZE - 1);
        if (cur1g < PAGE_1GB_SIZE) cur1g = PAGE_1GB_SIZE;

        while (cur1g < end) {
            uint32_t n = static_cast<uint32_t>(cur1g / PAGE_1GB_SIZE);
            if (n < PT_ENTRIES && n != PDPT_PD_ENTRY && pdpt[n] == 0) {
                pdpt[n] = cur1g | PDPT_1GB_PAGE_FLAGS;
            }
            cur1g += PAGE_1GB_SIZE;
        }
        reload_cr3();
    }
}

}  // namespace cinux::arch