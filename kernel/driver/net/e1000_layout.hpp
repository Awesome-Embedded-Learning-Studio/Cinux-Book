/**
 * @file    e1000_layout.hpp
 * @brief   Register offsets and descriptor words for the 82540em.
 *
 * One card, one word book: where each register lives inside the BAR0
 * window, which bits the driver actually flips, and the two sixteen-byte
 * legacy descriptor shapes the rings are built from. Facts only — no
 * behavior lives here. The driver reads registers as aligned dwords and
 * slices fields with the house bit vocabulary, never by re-casting an
 * unaligned pointer. The enable bits themselves are a lesson: transmit
 * and receive turn on at bit one, not bit zero.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver::net::e1000 {

/**
 * @brief   Register offsets inside the BAR0 memory window, in bytes.
 * @since   0.6.0
 * @ingroup kernel_driver
 */

/// Device control: reset, set-link-up.
inline constexpr uint32_t kRegCtrl   = 0x0000;
/// Device status: link and speed answers.
inline constexpr uint32_t kRegStatus = 0x0008;
/// EEPROM read: request a word, wait for the answer.
inline constexpr uint32_t kRegEerd   = 0x0014;
/// Interrupt mask clear: one stops every source.
inline constexpr uint32_t kRegImc    = 0x00D8;
/// Receive control: enable and acceptance.
inline constexpr uint32_t kRegRctl   = 0x0100;
/// Transmit control: enable.
inline constexpr uint32_t kRegTctl   = 0x0400;
/// Receive descriptor base, low half.
inline constexpr uint32_t kRegRdbal  = 0x2800;
/// Receive descriptor base, high half.
inline constexpr uint32_t kRegRdbah  = 0x2804;
/// Receive descriptor ring length.
inline constexpr uint32_t kRegRdlen  = 0x2808;
/// Receive head: where the card filled to.
inline constexpr uint32_t kRegRdh    = 0x2810;
/// Receive tail: where the driver drained to.
inline constexpr uint32_t kRegRdt    = 0x2818;
/// Transmit descriptor base, low half.
inline constexpr uint32_t kRegTdbal  = 0x3800;
/// Transmit descriptor base, high half.
inline constexpr uint32_t kRegTdbah  = 0x3804;
/// Transmit descriptor ring length.
inline constexpr uint32_t kRegTdlen  = 0x3808;
/// Transmit head: where the card consumed to.
inline constexpr uint32_t kRegTdh    = 0x3810;
/// Transmit tail: where the driver offered to.
inline constexpr uint32_t kRegTdt    = 0x3818;
/// Receive address lane zero: the kernel's unicast MAC.
inline constexpr uint32_t kRegRal    = 0x5400;
/// Receive address half zero: upper MAC bytes plus the valid bit.
inline constexpr uint32_t kRegRah    = 0x5404;

/**
 * @brief   Bits the driver flips, one fact per line.
 * @since   0.6.0
 * @ingroup kernel_driver
 */

/// Full device reset, self-clearing.
inline constexpr uint32_t kCtrlReset           = 0x1;
/// Ask the PHY to negotiate the link.
inline constexpr uint32_t kCtrlSetLinkUp       = 0x40;
/// Start one EEPROM read; the card clears it when done.
inline constexpr uint32_t kEerdStart           = 0x1;
/// The answer word has landed in the data bits.
inline constexpr uint32_t kEerdDone            = 0x10;
/// Word address rides bits eleven through eight.
inline constexpr uint32_t kEerdAddressShift    = 8;
/// The answer rides the top half of the register.
inline constexpr uint32_t kEerdDataShift       = 16;
/// Reception on, bit one.
inline constexpr uint32_t kRctlEnable          = 0x2;
/// Accept broadcast frames.
inline constexpr uint32_t kRctlBroadcastAccept = 0x8000;
/// Strip the trailing CRC before DMA.
inline constexpr uint32_t kRctlStripCrc        = 0x4000000;
/// Transmission on, bit one.
inline constexpr uint32_t kTctlEnable          = 0x2;
/// Pad frames shorter than the minimum.
inline constexpr uint32_t kTctlPadShortPackets = 0x8;
/// Without it a receive address entry stays inert.
inline constexpr uint32_t kRahAddressValid     = 0x80000000;

/// The card wrote this receive descriptor back.
inline constexpr uint8_t  kRxStatusDone        = 0x01;
/// This descriptor ends the frame.
inline constexpr uint8_t  kRxStatusEndOfPacket = 0x02;
/// This descriptor ends the packet.
inline constexpr uint32_t kTxCmdEndOfPacket    = 0x01;
/// Insert the Ethernet CRC on the way out.
inline constexpr uint32_t kTxCmdInsertCrc      = 0x02;
/// Ask for a writeback, so reuse can see completion.
inline constexpr uint32_t kTxCmdReportStatus   = 0x08;

/**
 * @brief   One legacy receive descriptor, sixteen bytes as the card sees it.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct RxDescriptor {
    /// Buffer address the card DMA'd into.
    uint64_t address;
    /// How many bytes arrived.
    uint16_t length;
    /// Hardware checksum leftover, unread here.
    uint16_t checksum;
    /// Descriptor done and end-of-packet bits.
    uint8_t  status;
    /// Reserved byte between status and errors.
    uint8_t  reserved;
    /// Error flags from the card.
    uint8_t  errors;
    /// VLAN tag, unread here.
    uint8_t  vlan;
};
static_assert(sizeof(RxDescriptor) == 16);

/**
 * @brief   One legacy transmit descriptor, sixteen bytes as the card sees it.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct TxDescriptor {
    /// Buffer address the card DMAs from.
    uint64_t address;
    /// How many bytes to send.
    uint16_t length;
    /// Checksum offset start, unused by this driver.
    uint8_t  checksum_offset;
    /// Command bits: end, insert-CRC, report-status.
    uint8_t  command;
    /// Writeback status, unread by this driver.
    uint8_t  status;
    /// Checksum start, unused by this driver.
    uint8_t  checksum_start;
    /// VLAN tag field, unused by this driver.
    uint16_t special;
};
static_assert(sizeof(TxDescriptor) == 16);

}  // namespace cinux::driver::net::e1000
