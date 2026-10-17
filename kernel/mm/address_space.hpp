/**
 * @file    address_space.hpp
 * @brief   One world of translations: a PML4 the kernel can enter.
 *
 * An AddressSpace owns a fresh top-level table whose kernel half
 * (PML4 entries 256..511) is copied from the kernel's own root, so
 * kernel code keeps running no matter which world is active, while the
 * user half starts empty and grows only through this space's own
 * mappings. The owning process lifecycle ends here now: teardown
 * retires the whole user subtree back to the frame allocator, so this
 * face covers the full life — build, mirror, map, translate, activate,
 * retire.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/ref_count.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"

namespace cinux::mm {

/**
 * @brief   A page-table world of its own.
 * @note    Not a singleton: one instance per world, the process being
 *          the future owner. Lives on the heap under shared ownership:
 *          a fork copies the world, a clone with CLONE_VM points its
 *          whole thread group at one, and the last task holding it
 *          tears the user half down.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
class AddressSpace : public cinux::base::RefCount<> {
public:
    /**
     * @brief         Builds the root table: one ledger page, zeroed,
     *                kernel half mirrored from the live root.
     *
     * @return        true when the space is ready to map into.
     * @note          Nothing switches yet — the machine stays wherever
     *                it is; building happens entirely through the
     *                direct map.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool init();

    /**
     * @brief         Physical address of this space's root table.
     *
     * @return        The PML4 to hand to LoadCr3, 0 before init.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] unsigned long root() const;

    /**
     * @brief         Maps one 4 KiB page inside this space.
     *
     * @param[in]     virtual_address   Where the page appears here.
     * @param[in]     physical          The frame it shows.
     * @return        true when the walk wrote the final entry.
     * @note          Goes through the shared walk with the kernel
     *                world's table supply; the live CR3 does not matter.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool map(unsigned long virtual_address, unsigned long physical) const;

    /**
     * @brief         Maps one 4 KiB page ring 3 may reach.
     *
     * @param[in]     virtual_address   Where the page appears here.
     * @param[in]     physical          The frame it shows.
     * @param[in]     leaf_bits         Flags the final entry carries; the
     *                                  default keeps the historical shape,
     *                                  writable, while the fault path and
     *                                  the loader pass narrower terms.
     * @return        true when the walk wrote the final entry.
     * @note          Same walk carrying kUser on every level, so ring 3
     *                        can descend the whole path; without it the
     *                        page is kernel-only even in the low half.
     *                        Only the final entry takes leaf_bits: a
     *                        table pointer's write bit is a supply
     *                        detail, the leaf's is the page's law.
     * @warning       None
     * @throws        None
     * @since         0.4.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool map_user(unsigned long virtual_address, unsigned long physical,
                                cinux::arch::page::Entry leaf_bits =
                                    cinux::arch::page::kWritable | cinux::arch::page::kUser) const;

    /**
     * @brief         Translates an address through this space's tables.
     *
     * @param[in]     virtual_address   The byte to resolve.
     * @return        Physical address of the same byte, 0 when unmapped.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] unsigned long translate(unsigned long virtual_address) const;

    /**
     * @brief         Switches the machine into this space.
     *
     * @return        None
     * @note          Interrupts must be off across the switch; the
     *                kernel half mirror is what keeps the caller alive
     *                inside the new world, and LoadCr3(KernelPageRoot())
     *                is the road back.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    void activate() const;

    /**
     * @brief         Returns every frame and table of the user half.
     *
     * @return        None.
     * @note          The retiring call of an owning process: walks the
     *                      user half only — the kernel half is a mirror,
     *                      its frames belong to the kernel — freeing leaf
     *                      frames, the tables that routed to them, and
     *                      the root itself. The space returns to its
     *                      pre-init shape with root 0 and must not be
     *                      the live CR3 of any CPU when this runs.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    void teardown();

    /**
     * @brief         Tears the user half down unless already torn.
     *
     * @return        None.
     * @note          The RAII half of the lifecycle: a world that goes
     *                      out of scope returns its frames by itself, so
     *                      owners delete and nothing more. Explicit
     *                      teardown leaves root zero and this runs empty.
     * @warning       Must not be the live CR3 of any CPU at destruction.
     * @throws        None
     * @since         0.3.0
     * @ingroup       kernel_mm
     */
    ~AddressSpace() { teardown(); }

private:
    unsigned long root_ = 0;
};

}  // namespace cinux::mm
