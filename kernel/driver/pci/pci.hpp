/**
 * @file    pci.hpp
 * @brief   The PCI bus as a queryable table of found devices.
 *
 * Consumers ask by class, never by slot number: the bus driver walks
 * configuration space once during bring-up, keeps what answered in a
 * stable-slot table, and hands out pointers that stay valid for the
 * machine's lifetime. Which controller sits where is a firmware fact,
 * not a kernel decision, so no consumer names a device.
 *
 * @author  Charliechen114514
 * @date    2026-10-08
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <array>
#include <optional>

#include "cinux/container/slot_table.hpp"
#include "cinux/singleton.hpp"
#include "kernel/driver/pci/pci_layout.hpp"

namespace cinux::driver {

/**
 * @brief   One answered device on the PCI bus, as configuration space told it.
 * @note    Raw bytes stay raw: class codes this kernel never names still
 *          find a home in the table.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct PciDevice {
    uint8_t                 bus;             ///< Bus the device answered on.
    uint8_t                 slot;            ///< Device number on that bus.
    uint8_t                 func;            ///< Function within the device.
    uint16_t                vendor_id;       ///< Who siliconed it.
    uint16_t                device_id;       ///< Which silicon of theirs.
    uint8_t                 class_code;      ///< Class-base byte, raw.
    uint8_t                 subclass;        ///< Subclass byte under the class base.
    uint8_t                 prog_if;         ///< Register-level programming interface byte.
    uint8_t                 revision;        ///< Silicon revision byte.
    uint8_t                 header_type;     ///< Layout of the rest of configuration space.
    uint8_t                 interrupt_pin;   ///< INT pin in use, 0 when none is asserted.
    uint8_t                 interrupt_line;  ///< Routing target the firmware left behind.
    std::array<uint32_t, 6> bars;            ///< Six base address registers, raw and unread.
};

/**
 * @brief   The walked-once PCI bus.
 * @note    Meyers-singleton shape after Pit and Pmm; the whole state is
 *          the device table, which fills during bring-up and never
 *          changes again, so the zero-construction rule holds.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
class PciBus : public cinux::base::Singleton<PciBus> {
    friend class cinux::base::Singleton<PciBus>;

public:
    /**
     * @brief         Walks configuration space and reports what answered.
     *
     * @return        None
     * @note          One serial line per answered device; a full table
     *                      stops the walk and says so. Runs once, inside
     *                      the bring-up ladder, before interrupts open.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void init();

    /**
     * @brief         Finds the first device matching a class and subclass.
     *
     * @param[in]     base      Class-base byte to match.
     * @param[in]     subclass  Subclass byte to match under it.
     * @return        The matching device, or nullptr when none answered.
     * @note          The pointer aims into the stable table and stays
     *                      valid for the machine's lifetime.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] const PciDevice* find_by_class(PciClassCode base, uint8_t subclass) const;

    /**
     * @brief         Wakes one device for driving.
     *
     * @param[in]     device  The device the driver is about to command.
     * @return        None
     * @note          Memory decoding, bus mastering and interrupt
     *                      assertion come on together: a device holding
     *                      all three can answer register reads, DMA and
     *                      signal completion.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_driver
     */
    void enable_device(const PciDevice& device);

private:
    PciBus() = default;

    /// @brief How many answered devices the table keeps before the walk stops.
    static constexpr unsigned long kDeviceCapacity = 32;

    /**
     * @brief         Scans one slot: gates on function zero, then every
     *                      function that answers joins the table.
     *
     * @param[in]     bus    Bus the slot lives on.
     * @param[in]     slot   Slot to scan.
     * @return        How many functions joined, or nullopt when the table
     *                      ran out of room mid-slot.
     * @note          A slot whose function zero stays silent holds nothing,
     *                      so the other seven go unprobed.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_driver
     */
    std::optional<unsigned long> scan_slot(uint8_t bus, uint8_t slot);

    /**
     * @brief         Stores one answered device and reports it.
     *
     * @param[in]     device  The device as probed.
     * @return        True when the table had room.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool admit(const PciDevice& device);

    cinux::base::container::SlotTable<PciDevice, kDeviceCapacity> devices_;
};

}  // namespace cinux::driver
