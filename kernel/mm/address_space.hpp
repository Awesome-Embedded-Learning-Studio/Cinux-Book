/**
 * @file    address_space.hpp
 * @brief   One world of translations: a PML4 the kernel can enter.
 *
 * An AddressSpace owns a fresh top-level table whose kernel half
 * (PML4 entries 256..511) is copied from the kernel's own root, so
 * kernel code keeps running no matter which world is active, while the
 * user half starts empty and grows only through this space's own
 * mappings. Destruction and recursive recycling of the user subtree
 * wait for the owning process lifecycle; this face is the stable
 * core — build, mirror, map, translate, activate.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::mm {

/**
 * @brief   A page-table world of its own.
 * @note    Not a singleton: one instance per world, the process being
 *          the future owner. Place instances in static storage; the
 *          kernel has no stacks deep enough to waste on one.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
class AddressSpace {
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
     * @return        true when the walk wrote the final entry.
     * @note          Same walk carrying kUser on every level, so ring 3
     *                        can descend the whole path; without it the
     *                        page is kernel-only even in the low half.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool map_user(unsigned long virtual_address, unsigned long physical) const;

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

private:
    unsigned long root_ = 0;
};

}  // namespace cinux::mm
