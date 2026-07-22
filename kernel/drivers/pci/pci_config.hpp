/**
 * @file kernel/drivers/pci/pci_config.hpp
 * @brief PCI hardware constants and configuration definitions
 *
 * Contains all PCI configuration space constants, register offsets,
 * class codes, and BAR type masks.  Separated from the driver
 * interface to keep pci.hpp clean.
 *
 * Namespace: cinux::drivers::pci
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::pci {

// ============================================================
// PCI Configuration Space I/O Ports
// ============================================================

namespace PciPort {
constexpr uint16_t CONFIG_ADDRESS = 0xCF8;  ///< Configuration address port
constexpr uint16_t CONFIG_DATA    = 0xCFC;  ///< Configuration data port
}  // namespace PciPort

// ============================================================
// PCI Configuration Space Register Offsets (in bytes)
// ============================================================

namespace PciReg {
constexpr uint8_t VENDOR_ID            = 0x00;
constexpr uint8_t DEVICE_ID            = 0x02;
constexpr uint8_t COMMAND              = 0x04;
constexpr uint8_t STATUS               = 0x06;
constexpr uint8_t REVID                = 0x08;
constexpr uint8_t PROG_IF              = 0x09;
constexpr uint8_t SUBCLASS             = 0x0A;
constexpr uint8_t CLASS_CODE           = 0x0B;
constexpr uint8_t HEADER_TYPE          = 0x0E;
constexpr uint8_t BAR0                 = 0x10;
constexpr uint8_t BAR1                 = 0x14;
constexpr uint8_t BAR2                 = 0x18;
constexpr uint8_t BAR3                 = 0x1C;
constexpr uint8_t BAR4                 = 0x20;
constexpr uint8_t BAR5                 = 0x24;
constexpr uint8_t CAPABILITIES_POINTER = 0x34;  ///< Pointer to first capability (bytes)
}  // namespace PciReg

// ============================================================
// PCI Class Codes
// ============================================================

namespace PciClass {
constexpr uint8_t MASS_STORAGE  = 0x01;
constexpr uint8_t AHCI_SUBCLASS = 0x06;
constexpr uint8_t SERIAL_BUS    = 0x0C;  ///< Serial bus controllers (USB, ...)
constexpr uint8_t USB_SUBCLASS  = 0x03;  ///< USB host/controller
constexpr uint8_t XHCI_PROG_IF  = 0x30;  ///< xHCI host controller interface
}  // namespace PciClass

// ============================================================
// PCI COMMAND Register Bits (offset 0x04)
// ============================================================

namespace PciCmd {
constexpr uint32_t IO_SPACE   = 0x01;  ///< Bit 0: I/O space access enable
constexpr uint32_t BUS_MASTER = 0x02;  ///< Bit 1: bus master enable (DMA)
constexpr uint32_t MEM_SPACE  = 0x04;  ///< Bit 2: memory space access enable (MMIO)
}  // namespace PciCmd

// ============================================================
// PCI STATUS Register Bits (offset 0x06)
// ============================================================

namespace PciStatus {
constexpr uint32_t CAP_LIST = 0x10;  ///< Bit 4: capabilities list present
}  // namespace PciStatus

// ============================================================
// PCI Capability IDs (capability list entry, byte 0)
// ============================================================

namespace PciCapId {
constexpr uint8_t POWER_MANAGEMENT = 0x01;  ///< Power Management
constexpr uint8_t MSI              = 0x05;  ///< Message Signalled Interrupts
constexpr uint8_t MSI_X            = 0x11;  ///< MSI-X
}  // namespace PciCapId

// ============================================================
// BAR Type Detection Masks
// ============================================================

constexpr uint32_t BAR_IO_SPACE     = 0x01;        ///< Bit 0: I/O space indicator
constexpr uint32_t BAR_TYPE_MASK    = 0x06;        ///< Bits 2:1: memory type
constexpr uint32_t BAR_TYPE_64      = 0x04;        ///< 64-bit memory mapping
constexpr uint32_t BAR_ADDR_MASK_32 = 0xFFFFFFF0;  ///< Lower 32-bit address mask

// ============================================================
// PCI Addressing Limits
// ============================================================

constexpr uint8_t  MAX_BUS        = 32;
constexpr uint8_t  MAX_SLOT       = 32;
constexpr uint8_t  MAX_FUNC       = 8;
constexpr uint8_t  BAR_COUNT      = 6;
constexpr uint16_t VENDOR_INVALID = 0xFFFF;

}  // namespace cinux::drivers::pci
