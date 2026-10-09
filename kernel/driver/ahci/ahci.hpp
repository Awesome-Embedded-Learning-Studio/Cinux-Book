/**
 * @file    ahci.hpp
 * @brief   The AHCI controller: one port, command slot zero, completion
 *          by interrupt.
 *
 * The driver finds its controller on the PCI bus by class, never by
 * slot: waking the device, mapping its register window and seating the
 * interrupt handler all live here. Completion arrives as an interrupt —
 * the waiter sleeps in hlt until the handler raises the flag, so no
 * cycle is spent polling the command register. One page holds the
 * command list and its table, one the receive FIS area, one the data
 * under transfer; the data page caps a command at eight sectors until
 * the block face takes over.
 *
 * @author  Charliechen114514
 * @date    2026-10-08
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <array>

#include "cinux/addr.hpp"
#include "cinux/singleton.hpp"
#include "kernel/driver/ahci/ahci_layout.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {

/**
 * @brief   The SATA host controller this kernel drives.
 * @note    Meyers-singleton shape after Pit and PciBus; all state fills
 *          during bring-up and the instance carries no constructor,
 *          so the zero-construction rule holds.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
class Ahci : public cinux::base::Singleton<Ahci> {
    friend class cinux::base::Singleton<Ahci>;

public:
    /**
     * @brief         Brings the controller up and asks it to identify.
     *
     * @return        True when a controller answered and identified.
     * @note          An absent controller is a machine fact, not a
     *                      boot failure: the serial line says so and
     *                      the climb continues. Runs after interrupts
     *                      open, because identification waits on one.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool init();

    /**
     * @brief         Reads blocks off the identified drive.
     *
     * @param[in]     first        First block to read.
     * @param[in]     span         Blocks to read, 1..kMaxSectorsPerCommand.
     * @param[out]    destination  Kernel memory the bytes land in; any
     *                             alignment, the driver bounces through
     *                             its own page.
     * @return        True when the drive moved every block without
     *                      complaint.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool read_blocks(Lba first, BlockSpan span, uint8_t* destination);

    /**
     * @brief         Writes blocks onto the identified drive.
     *
     * @param[in]     first   First block to write.
     * @param[in]     span    Blocks to write, 1..kMaxSectorsPerCommand.
     * @param[in]     source  Kernel memory the bytes leave from; any
     *                        alignment, same bounce.
     * @return        True when the drive took every block without
     *                      complaint.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool write_blocks(Lba first, BlockSpan span, const uint8_t* source);

    /**
     * @brief         Blocks the identified drive addresses.
     *
     * @return        Block ceiling, zero before identification.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] uint64_t block_count() const;

    /**
     * @brief         The identified drive's transfer unit in bytes.
     *
     * @return        The sector size, constant on this drive.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long block_size() const;

    /**
     * @brief         Orders this drive's writes before any later one.
     *
     * @return        True when the drive flushed its cache.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool flush();

    /**
     * @brief         Interrupt entry: the shared line's seat.
     * @note          Claims the interrupt only when a watched status
     *                bit is set, so a line-mate's assert passes through
     *                untouched.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void on_interrupt();

private:
    Ahci() = default;

    /**
     * @brief         Places one command in slot zero and launches it.
     *
     * @param[in]     command  ATA opcode.
     * @param[in]     lba      First sector.
     * @param[in]     count    Sector count.
     * @param[in]     write    True when data flows to the drive.
     * @return        False when the count is out of range.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool issue(uint32_t command, uint64_t lba, uint16_t count, bool write);

    /**
     * @brief         Waits until the interrupt published a verdict.
     *
     * @return        The published verdict for the issued command.
     * @note          A task parks on the completion; earlier contexts
     *                      sleep in EnableIrqAndHalt between checks.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool wait_for_completion();

    volatile HbaMem*      regs_       = nullptr;  ///< Controller window, unmapped means absent.
    unsigned long         port_index_ = 0;        ///< The one port this driver drives.
    interrupt::IrqLine    line_{};                ///< Line the controller signals on.
    cinux::base::PhysAddr command_page_;          ///< Command list and its table.
    cinux::base::PhysAddr fis_page_;              ///< Received-FIS area.
    cinux::base::PhysAddr data_page_;             ///< Bounce page under transfer.
    uint64_t              sector_count_ = 0;
    std::array<char, 41>  model_        = {};
    proc::Completion      completion_;  ///< One request's verdict, rearmed per command.
};

}  // namespace cinux::driver
