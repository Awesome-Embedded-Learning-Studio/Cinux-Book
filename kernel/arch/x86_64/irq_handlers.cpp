/**
 * @file kernel/arch/x86_64/irq_handlers.cpp
 * @brief Hardware IRQ handler implementations and IDT registration
 *
 * Provides C handler functions for all 16 hardware IRQ lines (IRQ0-15)
 * after PIC remapping.  Also provides irq_init() which registers all
 * IRQ ISR stubs into the IDT.
 *
 * Handler policy:
 *   - IRQ0 (PIT): forwards to PIT::irq0_handler() which tracks ticks
 *     and sends EOI internally.
 *   - IRQ1-15 (default): ack the interrupt via PIC EOI and ignore.
 *     This prevents unhandled IRQs from causing a Double Fault.
 *
 * Dependencies:
 *   - PIC must be initialised before any IRQ arrives
 *   - IDT must exist (irq_init() writes to it)
 */

#include <stdint.h>

#include "gdt.hpp"
#include "idt.hpp"
#include "irq_backend.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/usb/xhci_irq.hpp"
#include "kernel/lib/kprintf.hpp"
#include "pic.hpp"
#include "smp.hpp"

using cinux::arch::ExceptionVector;
using cinux::arch::GDT_KERNEL_CODE;
using cinux::arch::IDT;
using cinux::arch::IDTGateType;
using cinux::arch::IDTPrivilege;
using cinux::arch::InterruptFrame;
using cinux::arch::PIC;
using cinux::arch::g_idt;
using cinux::arch::make_idt_attr;
using cinux::lib::kprintf;

// ============================================================
// ISR stubs (defined in interrupts.S)
// ============================================================

extern "C" {
void irq0_stub();
void irq1_stub();
void irq2_stub();
void irq3_stub();
void irq4_stub();
void irq5_stub();
void irq6_stub();
void irq7_stub();
void irq8_stub();
void irq9_stub();
void irq10_stub();
void irq11_stub();
void irq12_stub();
void irq13_stub();
void irq14_stub();
void irq15_stub();
void reschedule_ipi_stub();  // F4-M4 M4-2: reschedule IPI (vector 0xE0)
void xhci_irq_stub();        // F5-M5 Batch 0C: xHCI event-ring MSI-X (vector 0x40)
void lapic_timer_stub();     // F5-M5 -smp: per-CPU LAPIC timer (vector 0x30)
}  // extern "C"

// ============================================================
// ISR-stub epilogue helpers (called from the ISR_IRQ macro in interrupts.S)
// ============================================================
// EOI is owned by the ISR stub (irq_eoi_isr below), NOT by individual C
// handlers.  A handler can no longer forget the EOI or call the wrong back-end
// (the keyboard PIC::send_eoi bug: in APIC mode the PIC EOI is a no-op, so
// vector 0x21 stayed in the LAPIC ISR, raised the processor priority, and
// blocked the PIT + every lower/equal-priority interrupt -- freezing the
// whole interrupt subsystem).  irq_exit() does the deferred-preempt switch
// the timer used to do inline, now run AFTER the stub has EOId.
// ============================================================

extern "C" void irq_eoi_isr(uint8_t irq) {
    cinux::arch::irq_eoi(irq);
}

// ============================================================
// IRQ routing table (constexpr, data-driven)
// ============================================================

struct IRQRoute {
    uint8_t   vector;
    IDT::Stub stub;
};

static constexpr IRQRoute k_irq_routes[] = {
    {0x20, irq0_stub},  {0x21, irq1_stub},  {0x22, irq2_stub},  {0x23, irq3_stub},
    {0x24, irq4_stub},  {0x25, irq5_stub},  {0x26, irq6_stub},  {0x27, irq7_stub},
    {0x28, irq8_stub},  {0x29, irq9_stub},  {0x2A, irq10_stub}, {0x2B, irq11_stub},
    {0x2C, irq12_stub}, {0x2D, irq13_stub}, {0x2E, irq14_stub}, {0x2F, irq15_stub},
};

static constexpr uint8_t kIRQAttr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);

// ============================================================
// Default IRQ handler (IRQ1-15)
// ============================================================

extern "C" {

/**
 * @brief Default handler for unconfigured IRQ lines
 *
 * No-op body: the ISR stub (ISR_IRQ macro) sends the EOI after the handler
 * returns, so an unexpected IRQ is still acknowledged and cannot stall
 * delivery.  Nothing else to do for an unconfigured line.
 *
 * @param frame  Interrupt stack frame (unused)
 */
void irq_default_handler(InterruptFrame* /*frame*/) {
    // EOI is owned by the ISR stub.
}

#ifndef CINUX_GUI
/**
 * @brief Default IRQ12 handler when GUI mode is disabled
 *
 * In non-GUI builds the mouse driver is not compiled, so we provide this stub.
 * In GUI builds the real implementation lives in mouse.cpp and overrides this
 * weak alias.  EOI is sent by the ISR stub in both cases.
 *
 * @param frame  Interrupt stack frame (unused)
 */
void mouse_irq12_handler(InterruptFrame* /*frame*/) {
    // EOI is owned by the ISR stub.
}
#endif

#ifndef CINUX_USB
/**
 * @brief Default xHCI IRQ handler when the USB driver is not compiled
 *
 * With CINUX_USB off, usb/xhci_irq.cpp is absent, so we provide this stub.
 * MSI-X is never programmed without the driver, so this handler should never
 * actually fire -- it exists only to satisfy the asm stub's link reference to
 * xhci_irq_handler.  (Mirrors the mouse #ifndef CINUX_GUI stub.)  EOI is sent
 * by the ISR stub.
 */
extern "C" void xhci_irq_handler(InterruptFrame* /*frame*/) {
    // EOI is owned by the ISR stub.
}
#endif

}  // extern "C"

// ============================================================
// irq_init() -- register all IRQ stubs into the IDT
// ============================================================

/**
 * @brief Register all hardware IRQ handlers into the IDT
 *
 * Installs ISR stubs for INT vectors 0x20-0x2F (IRQ0-15 after
 * PIC remapping).  All use kernel interrupt gates (DPL=0, IF cleared).
 *
 * Must be called after idt_init() and pic_init().
 */
extern "C" void irq_init() {
    kprintf("[IRQ] Registering IRQ handlers (0x20-0x2F)...\n");

    for (const auto& route : k_irq_routes) {
        g_idt.set_handler(static_cast<ExceptionVector>(route.vector), route.stub, GDT_KERNEL_CODE,
                          kIRQAttr, 0);
    }

    // Reschedule IPI (F4-M4 M4-2, vector 0xE0).  Registered into the shared IDT
    // so any CPU can take it; the handler is a LAPIC EOI no-op and the waking
    // AP's idle loop does the actual reschedule.  Dormant on a single-core
    // system (wake_idle_ap never sends it).
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::arch::kRescheduleIpiVector),
                      reschedule_ipi_stub, GDT_KERNEL_CODE, kIRQAttr, 0);

    // xHCI event-ring MSI-X interrupt (F5-M5 Batch 0C, vector kXhciIrqVector).
    // Registered at boot so the shared IDT has the entry before APs start.  The
    // handler is a no-op+counter until Batch 2C wires the controller, and MSI-X
    // is not programmed until then, so it never fires prematurely.
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::drivers::usb::kXhciIrqVector),
                      xhci_irq_stub, GDT_KERNEL_CODE, kIRQAttr, 0);

    // LAPIC timer (F5-M5 -smp, vector kLapicTimerVector).  Registered into the
    // shared IDT so APs can take it; the BSP is preempted by the PIT and never
    // arms its LAPIC timer, so this stays dormant there.  See ap_main().
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::arch::kLapicTimerVector),
                      lapic_timer_stub, GDT_KERNEL_CODE, kIRQAttr, 0);

    kprintf("[IRQ] All IRQ handlers registered.\n");
}
