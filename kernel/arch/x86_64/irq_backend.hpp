/**
 * @file kernel/arch/x86_64/irq_backend.hpp
 * @brief Interrupt back-end abstraction: PIC vs APIC EOI (F4-M2)
 *
 * Lets IRQ handlers call a single irq_eoi() that dispatches to the 8259 PIC or
 * the Local APIC depending on which back-end is active.  switch_to_apic() flips
 * the back-end during boot: mask the PIC, enable the LAPIC, and program the
 * I/O APIC to route the ISA IRQs we use (PIT/keyboard/mouse) onto the existing
 * IDT vectors (0x20/0x21/0x2C), honouring MADT IRQ source overrides.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

enum class IrqBackend : uint8_t {
    kPic,   ///< legacy 8259 PIC
    kApic,  ///< Local + I/O APIC
};

/// Active interrupt back-end (PIC until switch_to_apic()).
extern IrqBackend g_irq_backend;

/// End-Of-Interrupt via the active back-end.
/// PIC: send_eoi(irq). APIC: Local APIC EOI (edge-triggered ISA IRQs).
void irq_eoi(uint8_t irq);

/// Switch from the 8259 PIC to the APIC.
///
/// Masks the PIC, brings up the Local and I/O APICs, and routes ISA IRQ
/// 0/1/12 to vectors 0x20/0x21/0x2C on the BSP.  Must be called with
/// interrupts disabled (before sti).  On failure (no ACPI info / APIC map
/// failed) the system stays on the PIC.
void switch_to_apic();

}  // namespace cinux::arch
