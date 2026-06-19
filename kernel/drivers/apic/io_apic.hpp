/**
 * @file kernel/drivers/apic/io_apic.hpp
 * @brief I/O APIC driver (F4-M2)
 *
 * The I/O APIC routes external device interrupts (PCI INTx#, ISA IRQs) to Local
 * APICs.  Registers are accessed indirectly: write the register index to
 * IOREGSEL (offset 0x00), then read/write the data window IOWIN (offset 0x10).
 *
 * Like the Local APIC, the MMIO window is uncached (FLAG_PCD).  init() maps it
 * inside KMEM_MMIO (4 KB past the LAPIC window); bind() attaches to a mock base
 * for tests.  M2-2 provides the driver + redirect-table programming; the actual
 * PIC->APIC IRQ switch is M2-3.
 *
 * Namespace: cinux::drivers::apic
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::apic {

// Indirect-access register ports (byte offsets into the 4 KB window).
constexpr uint32_t kIoapicIORegSel = 0x00;  ///< register select (write index here)
constexpr uint32_t kIoapicIOWin    = 0x10;  ///< data window (read/write selected reg)

// I/O APIC register indices (selected via IOREGSEL).
constexpr uint32_t kIoapicRegId           = 0x00;  ///< ID (bits 24-27)
constexpr uint32_t kIoapicRegVer          = 0x01;  ///< version (bits 0-7), max entry (bits 16-23)
constexpr uint32_t kIoapicRegRedirectBase = 0x10;  ///< 2 regs (lo, hi) per redirection pin

// Redirection-entry low-word bit layout.
constexpr uint32_t kRedirectMaskBit      = 1u << 16;  ///< mask the pin
constexpr uint32_t kRedirectTriggerLevel = 1u << 15;  ///< 1 = level, 0 = edge
constexpr uint32_t kRedirectPolarityLow  = 1u << 13;  ///< 1 = active low, 0 = active high

class IOAPIC {
public:
    /// Attach to an already-mapped (or mock) window.  @p mmio_base points at
    /// IOREGSEL (offset 0x00); IOWIN is 0x10 bytes further.
    void bind(volatile uint32_t* mmio_base);

    /// Map the I/O APIC MMIO window (FLAG_PCD, uncached) and bind.
    /// @return true on success.
    bool init(uint64_t mmio_phys);

    /// Read/write a register by index (indirect via IOREGSEL/IOWIN).
    uint32_t read(uint32_t reg);
    void     write(uint32_t reg, uint32_t value);

    /// Program a redirection entry: route @p gsi to @p vector on Local APIC
    /// @p dest_apic_id, with optional polarity / trigger mode.  Destination mode
    /// is physical.
    void set_redirect(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id, bool active_low = false,
                      bool level_triggered = false);

    void mask(uint32_t gsi);    ///< set the mask bit on @p gsi's entry
    void unmask(uint32_t gsi);  ///< clear the mask bit

    uint32_t id();            ///< IOAPIC ID (bits 24-27 of reg 0)
    uint32_t version();       ///< version (bits 0-7 of reg 1)
    uint32_t max_redirect();  ///< max redirection entries = (ver >> 16) + 1

private:
    volatile uint32_t* base_ = nullptr;  ///< points at IOREGSEL (offset 0x00)
};

/// Global I/O APIC instance (first IOAPIC from MADT).
extern IOAPIC g_ioapic;

}  // namespace cinux::drivers::apic
