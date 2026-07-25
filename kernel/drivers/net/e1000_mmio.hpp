/**
 * @file kernel/drivers/net/e1000_mmio.hpp
 * @brief Intel e1000 (8254x) MMIO register offsets and control-bit constants
 *
 * The e1000 register block is sparse (the full 128 KB BAR0 also holds flash and
 * statistics regions we never touch), so offsets are named constants read and
 * written via E1000Controller::reg_read / reg_write -- NOT a packed struct
 * (unlike xHCI's dense capability block).  Mirrors the named-offset style of
 * pci_config.hpp.
 *
 * Covers 批a (reset + EEPROM MAC + link) and 批b (legacy RX ring).  The TX path
 * and the MSI interrupt path are deferred (82540em advertises MSI, not MSI-X).
 *
 * Namespace: cinux::drivers::net::e1000reg
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::net::e1000reg {

// ============================================================
// Control / status (批a)
// ============================================================

constexpr uint32_t CTRL   = 0x0000;  ///< Device Control
constexpr uint32_t STATUS = 0x0008;  ///< Device Status
constexpr uint32_t EECD   = 0x0010;  ///< EEPROM / Flash Control & Data
constexpr uint32_t EERD   = 0x0014;  ///< EEPROM Read
constexpr uint32_t IMC    = 0x00D8;  ///< Interrupt Mask Clear (write 1s to disarm)

// CTRL bits
constexpr uint32_t CTRL_FD      = 1U << 0;   ///< Full Duplex
constexpr uint32_t CTRL_SLU     = 1U << 6;   ///< Set Link Up
constexpr uint32_t CTRL_FRCSPD  = 1U << 11;  ///< Force Speed
constexpr uint32_t CTRL_FRCDPLX = 1U << 12;  ///< Force Duplex
constexpr uint32_t CTRL_RST     = 1U << 26;  ///< Device Reset (self-clears)

// STATUS bits
constexpr uint32_t STATUS_FD = 1U << 0;  ///< Full Duplex established
constexpr uint32_t STATUS_LU = 1U << 1;  ///< Link Up

// EERD bits (QEMU 8254x: word address in bits 11:8, data in bits 31:16)
constexpr uint32_t EERD_START      = 1U << 0;  ///< Start read (self-clears on DONE)
constexpr uint32_t EERD_DONE       = 1U << 4;  ///< Read complete
constexpr uint32_t EERD_ADDR_SHIFT = 8;        ///< word address field shift
constexpr uint32_t EERD_DATA_SHIFT = 16;       ///< word data field shift

// ============================================================
// Receive (批b)
// ============================================================

constexpr uint32_t RCTL  = 0x0100;  ///< Receive Control
constexpr uint32_t RDBAL = 0x2800;  ///< RX Descriptor Base Low
constexpr uint32_t RDBAH = 0x2804;  ///< RX Descriptor Base High
constexpr uint32_t RDLEN = 0x2808;  ///< RX Descriptor Length
constexpr uint32_t RDH   = 0x2810;  ///< RX Descriptor Head
constexpr uint32_t RDT   = 0x2818;  ///< RX Descriptor Tail
constexpr uint32_t RAL0  = 0x5400;  ///< Receive Address Low 0 (unicast MAC)
constexpr uint32_t RAH0  = 0x5404;  ///< Receive Address High 0
constexpr uint32_t MTA   = 0x5200;  ///< Multicast Table Array base

// RCTL bits
constexpr uint32_t RCTL_EN    = 1U << 1;   ///< Receiver Enable
constexpr uint32_t RCTL_UPE   = 1U << 3;   ///< Unicast Promiscuous Enable
constexpr uint32_t RCTL_BAM   = 1U << 15;  ///< Broadcast Accept Mode
constexpr uint32_t RCTL_SECRC = 1U << 26;  ///< Strip Ethernet CRC from RX

// ============================================================
// Transmit (批b: loopback self-test)
// ============================================================

constexpr uint32_t TCTL  = 0x0400;  ///< Transmit Control
constexpr uint32_t TDBAL = 0x3800;  ///< TX Descriptor Base Low
constexpr uint32_t TDBAH = 0x3804;  ///< TX Descriptor Base High
constexpr uint32_t TDLEN = 0x3808;  ///< TX Descriptor Length
constexpr uint32_t TDH   = 0x3810;  ///< TX Descriptor Head
constexpr uint32_t TDT   = 0x3818;  ///< TX Descriptor Tail

// TCTL bits
constexpr uint32_t TCTL_EN  = 1U << 1;  ///< Transmit Enable
constexpr uint32_t TCTL_PSP = 1U << 3;  ///< Pad Short Packets (< 60 B)

// Statistics (read for diagnostics)
constexpr uint32_t GPRC = 0x4074;  ///< Good Packets Received Count

}  // namespace cinux::drivers::net::e1000reg
