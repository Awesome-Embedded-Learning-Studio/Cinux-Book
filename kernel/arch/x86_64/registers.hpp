/**
 * @file    registers.hpp
 * @brief   The control registers the memory subsystem reads and writes.
 *
 * Thin inline wrappers around the few instructions C++ cannot name: CR3
 * owns the current page-table root, reloading it flushes non-global TLB
 * entries, and invlpg drops one page from the translation cache. Header
 * inline on purpose — each world instantiates them for free and no state
 * hides behind a translation unit.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch {

/// CR0 bit 16: supervisor writes honor read-only pages. Clear at reset,
/// which lets ring 0 pierce any W=0 mapping silently — copy-on-write and
/// every write-xor-execute plan both stand on this bit being set.
inline constexpr unsigned long long kCr0WriteProtect = 1ULL << 16U;

/// CR4 bit 20: supervisor mode execution prevention — ring 0 fetching
/// instructions from a user page faults instead of executing. A 2011+
/// feature, so the enable path asks CPUID before writing the bit.
inline constexpr unsigned long long kCr4Smep = 1ULL << 20U;

/// CR4 bit 21: supervisor mode access prevention — ring 0 touching a
/// user page at all faults unless RFLAGS.AC is set for the moment.
/// Gated by CPUID like SMEP, and served by the stac/clac window pair.
inline constexpr unsigned long long kCr4Smap = 1ULL << 21U;

/**
 * @brief         Reads CR0, the machine's coarse behavior switches.
 *
 * @return        The current CR0 image.
 * @note          One home for the reads the kernel-world makes; the boot
 *                worlds touch CR0 in their own copies.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
inline unsigned long long ReadCr0() {
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned long long value = 0;
    asm volatile("movq %%cr0, %0" : "=r"(value));
    return value;
}

/**
 * @brief         Writes CR0 wholesale.
 *
 * @param[in]     value   The new image.
 * @return        None.
 * @note          Callers flip named bits, never rebuild the word.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
inline void WriteCr0(unsigned long long value) {
    asm volatile("movq %0, %%cr0" : : "r"(value) : "memory");
}

/**
 * @brief         Reads CR4, the per-CPU feature switches.
 *
 * @return        The current CR4 image.
 * @note          CR4 belongs to one processor only: the read answers for
 *                the core it runs on, so the mechanism snapshot the boot
 *                prints per CPU takes one read per online core.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline unsigned long long ReadCr4() {
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned long long value = 0;
    asm volatile("movq %%cr4, %0" : "=r"(value));
    return value;
}

/**
 * @brief         Writes CR4 wholesale.
 *
 * @param[in]     value   The new image.
 * @return        None.
 * @note          Callers read, OR in named bits, and write back — never
 *                rebuild the word, the machine owns the rest of it. Bits
 *                the CPU does not know refuse the whole write with #GP,
 *                which is why SMEP and SMAP ask CPUID first.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline void WriteCr4(unsigned long long value) {
    asm volatile("movq %0, %%cr4" : : "r"(value) : "memory");
}

/**
 * @brief         Reads the physical address of the current page-table root.
 *
 * @return        Physical address held in CR3.
 * @note          The value is a live fact: after address-space switching
 *                begins it names whatever root is active, not necessarily
 *                the kernel's own.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
inline unsigned long ReadCr3() {
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned long value = 0;
    asm volatile("movq %%cr3, %0" : "=r"(value));
    return value;
}

/**
 * @brief         Reads CR2, the address the last page fault touched.
 *
 * @return        The faulting virtual address.
 * @note          Meaningful only while handling vector 14: the CPU writes
 *                it before entering the handler and nothing else keeps it,
 *                so the page-fault path reads it first thing.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
inline unsigned long ReadCr2() {
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned long value = 0;
    asm volatile("movq %%cr2, %0" : "=r"(value));
    return value;
}

/**
 * @brief         Reloads CR3 with its current value, flushing every
 *                non-global TLB entry.
 *
 * @return        None
 * @note          The blunt instrument: dropping whole address-space
 *                translations at once. Single-page removals use
 *                  InvalidatePage.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
inline void ReloadCr3() {
    asm volatile("movq %%cr3, %%rax\n\tmovq %%rax, %%cr3" : : : "rax", "memory");
}

/**
 * @brief         Loads a new page-table root into CR3, switching the
 *                machine to that address space.
 *
 * @param[in]     root   Physical address of the target PML4.
 * @return        None
 * @note          Everything the running code needs must already be
 *                mapped in the target space — kernel-half mirroring is
 *                what makes switching safe.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
inline void LoadCr3(unsigned long root) {
    asm volatile("movq %0, %%cr3" : : "r"(root) : "memory");
}

/**
 * @brief         Invalidates the TLB entry for one page.
 *
 * @param[in]     virtual_address   Any address inside the page to drop.
 * @return        None
 * @note          Required after unmapping or tightening permissions;
 *                  skipping it leaves stale translations in play.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
inline void InvalidatePage(unsigned long virtual_address) {
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

}  // namespace cinux::arch
