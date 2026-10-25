/**
 * @file    cow.hpp
 * @brief   Copy-on-write: shared frames, write faults, private copies.
 *
 * The engine fork stands on, built to run without fork: any mapping may
 * be marked shared, and a write fault against a shared mapping turns it
 * private again — copying the frame while others still look at it, or
 * simply re-arming write permission for the last sharer leaving. A
 * per-frame use count keeps the "who still owns this" question honest,
 * the piece the first act's version never had: there, a parent exit
 * freed frames a child was still reading. The fault side answers
 * through the one page-fault resolver seat the exception reporter
 * offers; everything here runs at fault depth — no sleeping, no
 * allocation beyond one frame, no locks beyond what the PMM itself
 * takes.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::mm {

class AddressSpace;
class VmaStore;

namespace cow {

/**
 * @brief         Mark one user mapping as sharing its frame.
 *
 * @param[in]     root    Page-table root the mapping lives under.
 * @param[in]     virtual_address   The page to mark; low bits ignored.
 * @return        true when a present 4 KiB mapping was marked.
 * @note          Drops write permission, sets the software share bit,
 *                and counts one more user of the frame. Mapping the
 *                same frame at several addresses and marking each is
 *                how a fork marks its inherited pages.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
[[nodiscard]] bool MarkPageShared(unsigned long root, unsigned long virtual_address);

/**
 * @brief         Offer one write fault to the copy-on-write engine.
 *
 * @param[in]     address     CR2, the byte the write touched.
 * @param[in]     error_code  The CPU-pushed fault code.
 * @return        true when the mapping was repaired — a private copy
 *                exists and the write may re-run; false when this fault
 *                is nobody's to fix.
 * @note          Walks the live root the fault happened under, so the
 *                caller must be the page-fault path itself. A shared
 *                page with company gets a fresh frame and a copy; the
 *                last sharer simply gets its write permission back.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
[[nodiscard]] bool TryResolvePageFault(unsigned long long address, unsigned long long error_code);

/**
 * @brief         Seat the engine at the exception reporter's fault door.
 *
 * @return        None.
 * @note          The resolver answers only faults it can repair;
 *          everything else falls through unchanged. Bring-up installs
 *          this once page tables exist; a machine without shared pages
 *          simply never hears back "true".
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
void InstallResolver();

/**
 * @brief         Give one frame back, honoring whoever still shares it.
 *
 * @param[in]     frame   The physical frame a retiring mapping showed.
 * @return        None.
 * @note          The teardown half of the use count: a frame with
 *                company stays put, the last user's release returns it
 *                to the allocator. Retiring a space calls this for
 *          every leaf it mapped, so a parent's death cannot yank a
 *                page a child still reads — the omission the first
 *                act carried.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
void ReleaseFrame(unsigned long long frame);

/**
 * @brief         Build a child address space sharing the parent's pages.
 *
 * @param[in]     parent   The world fork copies, read-only.
 * @return        The heap-owned child space, or nullptr when memory
 *                ran out.
 * @note          Walks the parent's user half, maps every 4 KiB leaf
 *                into the child at the same address, and marks both
 *                sides shared — read-only plus the software bit, the
 *                use count at two. The first write from either side
 *                then earns a private copy through the fault path.
 *                A large page in the user half stops the machine
 *                loudly: nothing that maps user memory makes one.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
[[nodiscard]] AddressSpace* CloneSharedSpace(const AddressSpace& parent,
                                             const VmaStore*     ranges = nullptr);

}  // namespace cow

}  // namespace cinux::mm
