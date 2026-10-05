/**
 * @file    vmm_config.hpp
 * @brief   VMM facts and knobs: the numbers the address-space bring-up
 *          is built from.
 *
 * The handoff doors cover only the kernel image's physical span, so new
 * page-table pages cannot come from the PMM — free pages there sit
 * outside every mapping the boot world left, and which page the ledger
 * would hand out is a policy, not a promise. Every table the bring-up
 * builds therefore comes from this fixed low span: the direct map's
 * PDPT and directories, the ioremap tables, all of it, sized for the
 * worst case the 16 GiB ceiling can ask. The span is consumed once and
 * then permanently owned as page tables, below the ledger's low-memory
 * withhold so the two never account for the same byte.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/region/memory_region.hpp"

namespace cinux::mm {

/**
 * @brief   Free low memory every bring-up page table is carved from.
 * @note    Above the ferry window, far below the kernel image; nothing
 *          in the boot chain is placed there after handoff. Sized for
 *          the worst case: one PDPT plus fifteen directories for 16 GiB
 *          of direct map, plus the ioremap pair, with headroom.
 * @since   0.2.0
 * @ingroup kernel_mm
 */
inline constexpr base::memory::MemoryRegion kSeedTablesSpan{0x20000, 0x40000};

}  // namespace cinux::mm
