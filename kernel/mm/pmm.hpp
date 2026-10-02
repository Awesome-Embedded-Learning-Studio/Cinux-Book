/**
 * @file    pmm.hpp
 * @brief   The physical memory ledger: announce face.
 *
 * Pmm turns the E820 map into page accounts — what boot stood on stays
 * spent, what firmware keeps stays closed, everything else is allocatable
 * in single pages or power-of-two runs. The implementation hides in
 * pmm.cpp; this header only announces.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/bit_ops/bitmap.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/mm/pmm_config.hpp"

namespace cinux::mm {

/**
 * @brief   One ledger over the machine's usable RAM.
 * @note    The bitmap storage is a member, so a Pmm is about half a
 *          megabyte of .bss at the current 16 GiB ceiling; place instances
 *          in static storage, not on small stacks.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
class Pmm {
public:
    /** @brief   Address type carried through the whole public face. */
    using PhysAddr = cinux::base::PhysAddr;
    /**
     * @brief         Builds the ledger from the handed-off memory map.
     *
     * @param[in]     entries        E820 records from BootInfo.
     * @param[in]     count          How many records follow.
     * @param[in]     kernel_base    Where the kernel image was delivered.
     * @param[in]     kernel_bytes   Memory extent of the kernel, bss included.
     * @return        true when the ledger is ready to allocate from.
     * @note          Usable RAM below kLowMemoryTop is withheld; the kernel
     *                image is marked spent where it lies.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool init(const cinux::boot::E820Entry* entries, uint32_t count,
                            PhysAddr kernel_base, unsigned long kernel_bytes);

    /** @brief         Hands out one page; zero address means nothing free. */
    [[nodiscard]] PhysAddr allocate_page() { return allocate_run(1); }

    /**
     * @brief         Hands out 2^order physically contiguous pages.
     *
     * @param[in]     order   Run size as a power of two, at most kMaxOrder.
     * @return        Base address of the run, or the zero address on
     *                refusal or exhaustion.
     * @note          Orders outside [0, kMaxOrder] are refused.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] PhysAddr allocate_pages(int order);

    /** @brief         Returns one page to the free pool. */
    void free_page(PhysAddr phys) { free_pages(phys, 0); }

    /**
     * @brief         Returns a whole run to the free pool.
     *
     * @param[in]     phys   Base address of the run.
     * @param[in]     order  The same order it was allocated with.
     * @return        None
     * @note          The order is the caller's contract; freeing with a
     *                wrong order corrupts the ledger.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    void free_pages(PhysAddr phys, int order);

    /** @brief         Pages currently free. */
    [[nodiscard]] unsigned long free_page_count() const;

    /** @brief         Pages the ledger governs at all. */
    [[nodiscard]] unsigned long managed_page_count() const;

private:
    [[nodiscard]] PhysAddr allocate_run(unsigned long page_count);

    cinux::base::bit::Bitmap   bitmap_;
    cinux::base::bit::WordMask words_[kPmmBitmapWords] = {};
    unsigned long              managed_pages_          = 0;
};

}  // namespace cinux::mm
