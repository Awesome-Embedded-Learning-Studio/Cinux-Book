/**
 * @file    msi_layout.hpp
 * @brief   The MSI and MSI-X capabilities, decoded from injected reads.
 *
 * Message-signaled interrupts keep their shape in configuration-space
 * capability headers. This piece reads what a device stated — which
 * entry count, which window holds the tables, how wide an address it
 * takes — through the same injected reader the capability walk uses,
 * so the host test world decodes headers no bus ever answered. Writing
 * the armed state back is the interrupt work's; nothing here touches
 * a port.
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

#include <array>

#include "kernel/driver/pci/pci_layout.hpp"

namespace cinux::driver::msi {

/// @brief The MSI capability's id in a capability list.
inline constexpr uint8_t kMsiCapId = 0x05;

/// @brief The MSI-X capability's id in a capability list.
inline constexpr uint8_t kMsixCapId = 0x11;

/**
 * @brief   What the MSI capability told us about itself.
 * @note    Address and data words come back as the device keeps them —
 *          zero until someone arms them — so a later read-back check
 *          compares against what was written, not against a guess.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct MsiFacts {
    bool     enabled;       ///< The message-signaling enable bit, as kept.
    bool     wide_address;  ///< Message addresses stretch to 64 bits.
    bool     maskable;      ///< Per-vector masking is supported.
    uint8_t  vector_cap;    ///< Log2 of how many vectors the device offers.
    uint32_t address;       ///< Current message address, low half.
    uint32_t address_high;  ///< Current message address, high half.
    uint16_t data;          ///< Current message data word.
};

/**
 * @brief   What the MSI-X capability told us about itself.
 * @note    Table size is the encoded field plus one: a device answering
 *          zero in the field keeps a single-entry table.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct MsixFacts {
    uint16_t table_size;      ///< How many table entries the device keeps.
    bool     enabled;         ///< The MSI-X enable bit, as kept.
    bool     masked;          ///< The function-wide mask bit, as kept.
    uint8_t  table_bir;       ///< Which base register holds the table.
    uint32_t table_offset;    ///< Where the table starts inside that window.
    uint8_t  pending_bir;     ///< Which base register holds the pending bits.
    uint32_t pending_offset;  ///< Where the pending bits start in the window.
};

/**
 * @brief   Where MSI-X keeps its two tables in the address space.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct MsixWindows {
    uint64_t table;    ///< The vector table's window base.
    uint64_t pending;  ///< The pending-bits array's window base.
};

/**
 * @brief         Decodes the MSI capability of one device.
 *
 * @param[in]     read    The configuration-space reader, injected.
 * @param[in]     context The reader's own context word, passed through.
 * @param[in]     spot    Which device to ask.
 * @param[out]    into    The capability's facts, filled on success.
 * @return        True when the device keeps an MSI capability.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool DecodeMsi(PciConfigRead read, const void* context, PciSpot spot, MsiFacts& into);

/**
 * @brief         Decodes the MSI-X capability of one device.
 *
 * @param[in]     read    The configuration-space reader, injected.
 * @param[in]     context The reader's own context word, passed through.
 * @param[in]     spot    Which device to ask.
 * @param[out]    into    The capability's facts, filled on success.
 * @return        True when the device keeps an MSI-X capability.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool DecodeMsix(PciConfigRead read, const void* context, PciSpot spot,
                              MsixFacts& into);

/**
 * @brief         Locates MSI-X's two tables in the address space.
 *
 * @param[in]     facts  The decoded capability, as DecodeMsix filled it.
 * @param[in]     bars   The device's six base registers, as the bus read them.
 * @return        Each table's window base, the register base plus the
 *                capability's offset inside it.
 * @note          A 64-bit register pairs its halves the way BarBase
 *                reads them, so the answer is ready for a memory window.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] MsixWindows ResolveMsixWindows(const MsixFacts&               facts,
                                             const std::array<uint32_t, 6>& bars);

}  // namespace cinux::driver::msi
