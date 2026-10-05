/**
 * @file    vmm.hpp
 * @brief   The kernel's own page tables: bring-up and the handoff record.
 *
 * BringUpAddressSpace is the one-shot ceremony that turns the boot
 * world's handoff doors into the published layout: the direct map of
 * RAM, the ioremap alias for the framebuffer, the user half dropped to
 * zero. It hands back the boot record from kernel-owned storage, so the
 * caller's reference survives the low half going away. MapFramebufferDoor
 * is the piece a test kernel can run alone when it needs the screen but
 * not the whole takeover.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/boot/boot_info.hpp"

namespace cinux::mm {

/**
 * @brief   The kernel world for the shared page walk.
 * @note    Table pages are reached through the direct map; fresh tables
 *          come from the PMM pre-zeroed, physical 0 meaning refusal.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
class KernelTables {
public:
    /**
     * @brief         Reaches one table page behind the direct map.
     *
     * @param[in]     physical   Physical address of the table page.
     * @return        Writable view of the table.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    cinux::arch::page::Entry* table(unsigned long physical);

    /**
     * @brief         Draws one zeroed page from the ledger as a new table.
     *
     * @return        Physical address of the fresh table, 0 on refusal.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    unsigned long take_page();
};

/**
 * @brief         Installs the framebuffer's ioremap alias into the live
 *                page tables.
 *
 * @param[in]     info   The boot handoff record carrying the framebuffer.
 * @return        None
 * @note          Seed pages from the low span; no PMM, no direct map,
 *                  no low-half teardown, so a test kernel can light the
 *                  screen without the full takeover.
 * @warning       Must run before any code dereferences the framebuffer
 *                through its ioremap alias.
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
void MapFramebufferDoor(const cinux::boot::BootInfo& info);

/**
 * @brief         Replaces the boot handoff doors with the published
 *                layout and archives the handoff record.
 *
 * @param[in]     handoff   The boot record, still readable through the
 *                          low-half window this call is about to drop.
 * @return        Pointer to the archived copy in kernel-owned storage,
 *                or nullptr when bring-up failed and the caller should
 *                stop.
 * @note          Requires Pmm::init first: upper direct-map tables are
 *                allocated from the ledger. Interrupts must be off.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
const cinux::boot::BootInfo* BringUpAddressSpace(const cinux::boot::BootInfo& handoff);

/**
 * @brief         The kernel's own page-table root, fixed at takeover.
 *
 * @return        Physical address of the kernel PML4.
 * @note          Capture of the moment switching began: once other
 *                address spaces activate, CR3 names the live one, while
 *                this stays the road home.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
unsigned long KernelPageRoot();

}  // namespace cinux::mm
