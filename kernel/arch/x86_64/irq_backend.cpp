/**
 * @file kernel/arch/x86_64/irq_backend.cpp
 * @brief Interrupt back-end abstraction implementation (F4-M2)
 */

#include "irq_backend.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/drivers/acpi/acpi.hpp"
#include "kernel/drivers/apic/io_apic.hpp"
#include "kernel/drivers/apic/local_apic.hpp"
#include "kernel/lib/kprintf.hpp"
#include "pic.hpp"

using cinux::lib::kprintf;

namespace cinux::arch {

IrqBackend g_irq_backend = IrqBackend::kPic;

void irq_eoi(uint8_t irq) {
    if (g_irq_backend == IrqBackend::kApic) {
        cinux::drivers::apic::g_lapic.eoi();
    } else {
        PIC::send_eoi(irq);
    }
}

namespace {

/// Spurious-interrupt vector for the LAPIC (enables the APIC in the SVR).
constexpr uint8_t kSpuriousVector = 0xFF;

/// Map an ISA IRQ to its Global System Interrupt, honouring MADT overrides.
/// QEMU's 'pc' machine typically overrides IRQ0 -> GSI2; the rest are 1:1.
uint32_t isa_irq_to_gsi(uint32_t isa_irq) {
    const auto& info = cinux::drivers::acpi::g_acpi_info;
    for (uint32_t i = 0; i < info.irq_override_count; ++i) {
        if (info.irq_overrides[i].source_irq == isa_irq) {
            return info.irq_overrides[i].global_irq;
        }
    }
    return isa_irq;
}

}  // namespace

void switch_to_apic() {
    const auto& info = cinux::drivers::acpi::g_acpi_info;
    if (info.local_apic_address == 0) {
        kprintf("[APIC] no LAPIC address from ACPI; staying on PIC\n");
        return;
    }

    // 1. Mask the 8259 PIC -- it is superseded by the APIC.
    PIC::disable_all();

    // 2. Bring up the Local APIC.
    if (!cinux::drivers::apic::g_lapic.init(info.local_apic_address)) {
        kprintf("[APIC] LAPIC map failed; staying on PIC\n");
        return;
    }
    cinux::drivers::apic::g_lapic.enable(kSpuriousVector);

    const uint8_t bsp = static_cast<uint8_t>(cinux::drivers::apic::g_lapic.id());

    // 3. Bring up the I/O APIC and route the ISA IRQs we use.  Vectors reuse
    //    the PIC remap (0x20/0x21/0x2C) so the existing IDT stubs/handlers
    //    work unchanged; only the EOI source flips (LAPIC instead of PIC).
    if (info.has_ioapic) {
        if (!cinux::drivers::apic::g_ioapic.init(info.ioapic_address)) {
            kprintf("[APIC] IOAPIC map failed; staying on PIC\n");
            return;
        }
        const uint32_t isa_irqs[3] = {0u, 1u, 12u};
        for (size_t i = 0; i < 3; ++i) {
            const uint32_t gsi    = isa_irq_to_gsi(isa_irqs[i]);
            const uint8_t  vector = static_cast<uint8_t>(0x20u + isa_irqs[i]);
            cinux::drivers::apic::g_ioapic.set_redirect(gsi, vector, bsp);
            cinux::drivers::apic::g_ioapic.unmask(gsi);
        }
    }

    // 4. Flip the EOI back-end.  From here, irq_eoi() targets the LAPIC.
    g_irq_backend = IrqBackend::kApic;
    kprintf("[APIC] switched to APIC (LAPIC id %u, IOAPIC @0x%lX)\n", static_cast<unsigned>(bsp),
            static_cast<unsigned long>(info.ioapic_address));
}

}  // namespace cinux::arch
