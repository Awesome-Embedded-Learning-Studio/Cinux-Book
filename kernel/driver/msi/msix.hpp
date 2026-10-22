/**
 * @file    msix.hpp
 * @brief   Arming one MSI-X table entry: the write side of the family.
 *
 * Decode lives in msi_layout; this piece writes. One table entry takes
 * its message address and data under a four-step protocol — mask first,
 * so a half-written entry cannot fire, then the pair, then a read-back
 * as the fence, then unmask only when the device kept what it was
 * given. The entry type wears volatile, so the same code drives a host
 * test's memory and a device's uncached window. The message address
 * word is the xAPIC convention, shared by MSI and MSI-X alike.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver::msi {

/**
 * @brief   One 16-byte MSI-X table entry, as the device keeps it.
 * @note    Volatile throughout: the window is uncached device memory,
 *          and every field read or write is the protocol itself.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct MsixEntry {
    volatile uint64_t address;  ///< Message address, the destination word.
    volatile uint32_t data;     ///< Message data, the vector it carries.
    volatile uint32_t control;  ///< Control word, bit 0 masks the entry.
};

static_assert(sizeof(MsixEntry) == 16, "one msi-x entry keeps its sixteen bytes");

/**
 * @brief         The xAPIC message address one vector travels to.
 *
 * @param[in]     destination   Physical APIC id of the receiving core.
 * @return        The address word a device programs: the local
 *                controller's message window with the destination in
 *                its field.
 * @note          Fixed delivery, edge triggered: the convention bits
 *                stay zero, so the base and the destination are the
 *                whole word.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] uint64_t XapicMessageAddress(uint8_t destination);

/**
 * @brief         Whether an entry carries exactly the pair asked.
 *
 * @param[in]     entry    The entry as the device keeps it now.
 * @param[in]     address  The address that was written.
 * @param[in]     data     The data word that was written.
 * @return        True when both fields read back unchanged.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool EntryLatched(const volatile MsixEntry& entry, uint64_t address, uint32_t data);

/**
 * @brief         Arms one entry with the four-step protocol.
 *
 * @param[in,out] entry    The entry being armed.
 * @param[in]     address  Message address, from XapicMessageAddress.
 * @param[in]     data     Message data, the seated vector's raw word.
 * @return        True when the entry kept the pair and now stands
 *                unmasked and live.
 * @note          Failure keeps the mask on: an entry that dropped its
 *                write stays silent rather than firing a stale message.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
bool ArmMsixEntry(volatile MsixEntry& entry, uint64_t address, uint32_t data);

}  // namespace cinux::driver::msi
