/**
 * @file kernel/drivers/pci/pci.hpp
 * @brief PCI configuration space access and device enumeration
 *
 * Provides a PCI class that encapsulates configuration space
 * read/write via the 0xCF8/0xCFC address-data register pair,
 * and a high-level enumeration method to locate AHCI host bus
 * adapters (class 0x01, subclass 0x06).
 *
 * Usage:
 *   PCI pci;
 *   pci.init();
 *   PCIDevice ahci_dev;
 *   if (pci.find_ahci(ahci_dev)) { ... }
 *
 * Namespace: cinux::drivers::pci
 */

#pragma once

#include <stdint.h>

#include "pci_config.hpp"

namespace cinux::drivers::pci {

// ============================================================
// PCI Device Descriptor
// ============================================================

/**
 * @brief Decoded PCI device/function descriptor
 *
 * Bundles the Bus/Slot/Func addressing triple and the decoded
 * header fields (vendor, device, class, subclass, prog_if,
 * and up to six BAR values) for a single PCI function.
 */
struct PCIDevice {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint32_t bar[BAR_COUNT];
};

/**
 * @brief True iff the class/subclass/prog_if triple is an xHCI host controller
 *
 * xHCI = class 0x0C (serial bus) / subclass 0x03 (USB) / prog_if 0x30 (xHCI).
 * prog_if is MANDATORY: 0x00=UHCI, 0x10=OHCI, 0x20=EHCI would all match a
 * class+subclass-only test.  Pure (host-testable).
 */
constexpr bool is_xhci_device(uint8_t cls, uint8_t sub, uint8_t prog_if) {
    return cls == PciClass::SERIAL_BUS && sub == PciClass::USB_SUBCLASS &&
           prog_if == PciClass::XHCI_PROG_IF;
}

/**
 * @brief True iff vendor/device identify an Intel e1000 NIC
 *
 * Matched on vendor + device (NOT PCI class 0x02) so it does not bind virtio-net
 * or other network controllers reserved for later milestones.  Covers the
 * 82540em (QEMU -device e1000 default) and the common e1000/e1000e variants.
 * Pure (host-testable).
 */
constexpr bool is_e1000_device(uint16_t vendor, uint16_t device) {
    if (vendor != 0x8086) {
        return false;
    }
    switch (device) {
    case 0x100E:  // 82540em (QEMU -device e1000 default)
    case 0x100F:  // 82541PI
    case 0x10EA:  // 82545EM
    case 0x10D3:  // 82574L (-device e1000e)
    case 0x10EF:  // 82578DM
        return true;
    default:
        return false;
    }
}

/**
 * @brief True iff the class/subclass identify an NVMe controller
 *
 * NVMe = class 0x01 (mass storage) / subclass 0x08 (NVM controller).
 * prog_if 0x02 (NVMe over PCIe) is NOT required: subclass 0x08 already
 * uniquely identifies NVM controllers, and older NVMe chips may leave prog_if
 * unset.  Pure (host-testable).
 */
constexpr bool is_nvme_device(uint8_t cls, uint8_t sub) {
    return cls == PciClass::MASS_STORAGE && sub == PciClass::NVME_SUBCLASS;
}

/**
 * @brief True iff vendor/device identify a VirtIO Block device
 *
 * Matched on vendor 0x1AF4 + device (legacy 0x1001 OR modern 0x1042).  QEMU's
 * -device virtio-blk-pci defaults to transitional (PCI device_id = 0x1001) but
 * still exposes the modern capability list, so both IDs must be accepted; the
 * transport is always driven modern.  Pure (host-testable).
 */
constexpr bool is_virtio_block_device(uint16_t vendor, uint16_t device) {
    return vendor == VirtioPci::VENDOR &&
           (device == VirtioPci::BLOCK_LEGACY || device == VirtioPci::BLOCK_MODERN);
}

/**
 * @brief True iff vendor/device identify a VirtIO Net NIC
 *
 * Matched on vendor 0x1AF4 + device (legacy 0x1000 OR modern 0x1041).  Same
 * transitional/modern rationale as is_virtio_block_device().  Does NOT bind the
 * e1000 NIC (vendor 0x8086), so the two can coexist for an A/B comparison.
 * Pure (host-testable).
 */
constexpr bool is_virtio_net_device(uint16_t vendor, uint16_t device) {
    return vendor == VirtioPci::VENDOR &&
           (device == VirtioPci::NET_LEGACY || device == VirtioPci::NET_MODERN);
}

// ============================================================
// PCI Class
// ============================================================

/**
 * @brief PCI configuration space access and device enumeration
 *
 * Encapsulates the 0xCF8/0xCFC address-data register pair for
 * reading and writing PCI configuration space.  Provides device
 * enumeration and BAR decoding methods.
 */
class PCI {
public:
    /**
     * @brief Initialise the PCI subsystem
     *
     * Scans all bus/slot/function combinations and logs a summary
     * of found devices to the kernel console.
     */
    void init();

    /**
     * @brief Read a 32-bit dword from PCI configuration space
     *
     * Constructs the 32-bit address word (enable bit | bus | slot |
     * func | register offset) and writes it to port 0xCF8, then
     * reads the result from port 0xCFC.
     *
     * @param bus      PCI bus number (0-255)
     * @param slot     Device/slot number on the bus (0-31)
     * @param func     Function number (0-7)
     * @param offset   Register offset (must be dword-aligned)
     * @return         The 32-bit value read from the configuration register
     */
    static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

    /**
     * @brief Write a 32-bit dword to PCI configuration space
     *
     * @param bus      PCI bus number
     * @param slot     Device/slot number
     * @param func     Function number
     * @param offset   Register offset (must be dword-aligned)
     * @param value    The 32-bit value to write
     */
    static void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

    /**
     * @brief Enumerate PCI buses and locate an AHCI HBA
     *
     * Scans all bus/slot/function combinations looking for a device
     * with class_code == 0x01 (mass storage) and subclass == 0x06
     * (SATA / AHCI).  The first match is written to @p out.
     *
     * @param out  Reference to a PCIDevice to fill with the match
     * @return     true if an AHCI device was found, false otherwise
     */
    bool find_ahci(PCIDevice& out) const;

    /**
     * @brief Enumerate PCI buses and locate an xHCI host controller
     *
     * Scans all bus/slot/function combinations for a device whose class /
     * subclass / prog_if match is_xhci_device() (0x0C / 0x03 / 0x30).  The
     * first match is written to @p out with its BARs decoded.
     *
     * @param out  Reference to a PCIDevice to fill with the match
     * @return     true if an xHCI controller was found, false otherwise
     */
    bool find_xhci(PCIDevice& out) const;

    /**
     * @brief Enumerate PCI buses and locate an Intel e1000 NIC
     *
     * Scans for a device matching is_e1000_device() (vendor 0x8086 + an e1000
     * device ID).  The first match is written to @p out with its BARs decoded.
     *
     * @param out  Reference to a PCIDevice to fill with the match
     * @return     true if an e1000 NIC was found, false otherwise
     */
    bool find_e1000(PCIDevice& out) const;

    /**
     * @brief Enumerate PCI buses and locate an NVMe controller
     *
     * Scans for a device matching is_nvme_device() (class 0x01 / subclass 0x08).
     * The first match is written to @p out with BARs decoded; BAR0 holds the
     * controller register window.
     *
     * @param out  Reference to a PCIDevice to fill with the match
     * @return     true if an NVMe controller was found, false otherwise
     */
    bool find_nvme(PCIDevice& out) const;

    /**
     * @brief Enumerate PCI buses and locate a VirtIO Block device
     *
     * Scans for a device matching is_virtio_block_device() (vendor 0x1AF4 +
     * device 0x1001/0x1042).  The first match is written to @p out with BARs
     * decoded; the modern capability list (common_cfg/notify/isr/device_cfg)
     * is walked later by the VirtIO transport layer.
     *
     * @param out  Reference to a PCIDevice to fill with the match
     * @return     true if a VirtIO Block device was found, false otherwise
     */
    bool find_virtio_block(PCIDevice& out) const;

    /**
     * @brief Enumerate PCI buses and locate a VirtIO Net NIC
     *
     * Scans for a device matching is_virtio_net_device() (vendor 0x1AF4 +
     * device 0x1000/0x1041).  The first match is written to @p out with BARs
     * decoded.
     *
     * @param out  Reference to a PCIDevice to fill with the match
     * @return     true if a VirtIO Net NIC was found, false otherwise
     */
    bool find_virtio_net(PCIDevice& out) const;

    /**
     * @brief Read all six BAR values from a PCI device
     *
     * Handles both 32-bit and 64-bit memory BARs: when a 64-bit
     * BAR is encountered, the next BAR register is consumed as the
     * upper 32 bits and the following BAR index is skipped.
     *
     * @param dev  The PCI device (bus/slot/func) to read BARs from
     */
    static void read_bars(PCIDevice& dev);

private:
    /**
     * @brief Scan a single PCI function and fill in the device descriptor
     *
     * Reads vendor/device/class/subclass/prog_if/header_type from
     * the configuration space of the given bus/slot/func.  Returns
     * false if the vendor ID is 0xFFFF (empty slot).
     *
     * @param bus   Bus number
     * @param slot  Slot number
     * @param func  Function number
     * @param dev   Output device descriptor
     * @return      true if a device was found at this location
     */
    static bool scan_function(uint8_t bus, uint8_t slot, uint8_t func, PCIDevice& dev);

    /// Triple-nested bus/slot/func scan; on the first device where @p match
    /// returns true, read_bars() + fill @p out + log "<name> found ... BAR<log_bar>"
    /// and return true.  Shared by find_ahci / find_xhci / find_e1000 (formerly
    /// three byte-for-byte-identical 25-line scans).
    bool find_device(const char* name, uint8_t log_bar,
                     bool (*match)(const PCIDevice&), PCIDevice& out) const;
};

}  // namespace cinux::drivers::pci
