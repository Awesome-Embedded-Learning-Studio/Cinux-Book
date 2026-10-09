/**
 * @file    pci_layout.hpp
 * @brief   PCI configuration-space types and the encoders that pack them.
 *
 * The bus speaks through one address word and dword-wide registers; the
 * encoders that build and read those words live in pci_layout.cpp, one
 * TU both worlds compile, so the host test world checks the arithmetic
 * without a bus in sight. Nothing here touches a port: the bus driver
 * in pci.cpp owns the cycles.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.3
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <array>

namespace cinux::driver {

/// @brief Subclass under mass storage: the PIIX IDE controller the boot disk hangs on.
inline constexpr uint8_t kPciSubclassIde = 0x01;

/// @brief Subclass under mass storage: the AHCI controller this station drives.
inline constexpr uint8_t kPciSubclassAhci = 0x06;

/// @brief Subclass under mass storage: NVMe, the second consumer waiting at a later station.
inline constexpr uint8_t kPciSubclassNvme = 0x08;

/**
 * @brief   Class-base bytes the kernel names; the raw byte stays raw.
 * @note    A device may carry a class this list never spells, so the bus
 *          table stores the plain byte and only queries go through these.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
enum class PciClassCode : uint8_t {
    /// class 0x01: mass storage controllers
    kMassStorage   = 0x01,
    /// class 0x02: network controllers
    kNetwork       = 0x02,
    /// class 0x03: display controllers
    kDisplay       = 0x03,
    /// class 0x04: multimedia devices
    kMultimedia    = 0x04,
    /// class 0x05: memory controllers
    kMemory        = 0x05,
    /// class 0x06: bridge devices, host and ISA among them
    kBridge        = 0x06,
    /// class 0x07: communication controllers
    kCommunication = 0x07,
    /// class 0x08: generic system peripherals
    kSystem        = 0x08,
    /// class 0x09: input devices
    kInput         = 0x09,
    /// class 0x0A: docking stations
    kDocking       = 0x0A,
    /// class 0x0B: processors
    kProcessor     = 0x0B,
    /// class 0x0C: serial bus controllers, USB among them
    kSerialBus     = 0x0C,
};

/**
 * @brief   What one base address register decodes into.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
enum class PciBarKind : uint8_t {
    /// register reads zero: nothing is mapped here
    kUnused,
    /// the device answers on port I/O space
    kIo,
    /// 32-bit memory window, the common case on this machine
    kMemory32,
    /// memory window whose high half lives in the next register
    kMemory64,
};

/**
 * @brief   One decoded base address register.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct PciBar {
    uint64_t   base;          ///< Decoded base address, ready to remap.
    PciBarKind kind;          ///< Which space and width the window lives in.
    bool       prefetchable;  ///< Memory the CPU may read ahead in.
};

/**
 * @brief         Packs one configuration-space location into the address word.
 *
 * @param[in]     bus    Bus number, 0..255 in the encoding.
 * @param[in]     slot   Device number on that bus, 0..31.
 * @param[in]     func   Function within the device, 0..7.
 * @param[in]     offset Byte address of the wanted register; the engine
 *                      fetches the dword that contains it.
 * @return        The dword to write to the configuration address port.
 * @note          Bit 31 enables configuration cycles; the register field
 *                      carries the dword index, so a misaligned offset
 *                      falls back to its containing dword.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] uint32_t MakeConfigAddress(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/**
 * @brief         Decodes the low register of a base address window.
 *
 * @param[in]     raw   The register as the bus read it.
 * @return        Kind, base and prefetchability of the window.
 * @note          A 64-bit window takes its high half from the next
 *                      register; see BarBase for the pair-aware read.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] PciBar DecodeBar(uint32_t raw);

/**
 * @brief         Reads the full base of one window, pairing 64-bit halves.
 *
 * @param[in]     bars   The six raw registers as the bus read them.
 * @param[in]     index  Which window to read, 0..5.
 * @return        The window's base address.
 * @note          A 64-bit window takes bars[index + 1] as its high half.
 *                      A 64-bit window at index 5 is malformed; its high
 *                      half reads as zero rather than trapping the boot.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] uint64_t BarBase(const std::array<uint32_t, 6>& bars, unsigned long index);

}  // namespace cinux::driver
