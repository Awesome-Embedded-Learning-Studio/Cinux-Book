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
