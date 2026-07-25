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
#include "kernel/drivers/apic/local_apic.hpp"
#include "kernel/drivers/nvme/nvme.hpp"  // F5-M3 batch 4: kNvmeIrqVector
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/usb/xhci_irq.hpp"
#include "kernel/drivers/virtio/virtio.hpp"  // F5-M2 batch 3: kVirtioBlkIrqVector
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
void shootdown_ipi_stub();   // B3 defect C: TLB shootdown IPI (vector 0xE1)
void xhci_irq_stub();        // F5-M5 Batch 0C: xHCI event-ring MSI-X (vector 0x40)
void nvme_irq_stub();        // F5-M3 batch 4: NVMe MSI-X (vector 0x41)
void virtio_blk_irq_stub();  // F5-M2 batch 3: VirtIO-blk MSI-X (vector 0x42)
void virtio_net_irq_stub();  // F5-M2 batch 5: VirtIO-net MSI-X (vector 0x43)
void lapic_timer_stub();     // F5-M5 -smp: per-CPU LAPIC timer (vector 0x30)
void net_timer_stub();       // F5-M6: e1000 RX-poll wakeup timer (vector 0x30, test kernel)
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

/**
 * @brief No-op LAPIC timer handler for the e1000 RX poll path (F5-M6).
 *
 * Armed by the test kernel (main_test.cpp) so e1000 RX poll can sti+hlt
 * between polls: hlt lets QEMU's main loop deliver SLIRP replies into the
 * ring, and this IRQ wakes the CPU.  EOIs the LAPIC directly -- NOT via
 * irq_eoi(), which would dispatch to the 8259 in the test kernel (it never
 * calls switch_to_apic) and fail to ack the LAPIC timer.  No
 * Scheduler::tick(): it must not perturb the test suite.
 */
void net_timer_handler(InterruptFrame* /*frame*/) {
    cinux::drivers::apic::g_lapic.eoi();
}

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
                          kIRQAttr, 2);
    }

    // Reschedule IPI (F4-M4 M4-2, vector 0xE0).  Registered into the shared IDT
    // so any CPU can take it; the handler is a LAPIC EOI no-op and the waking
    // AP's idle loop does the actual reschedule.  Dormant on a single-core
    // system (wake_idle_ap never sends it).
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::arch::kRescheduleIpiVector),
                      reschedule_ipi_stub, GDT_KERNEL_CODE, kIRQAttr, 2);

    // TLB shootdown IPI (B3 defect C, vector 0xE1).  Registered into the shared
    // IDT so any CPU can take it; the handler (tlb.cpp) invlpg's + decrements
    // acks_remaining, and the ISR_IRQ stub EOIs.  Dormant until stage 2 wires
    // handle_cow_fault -> tlb_shootdown_page (the mechanism test fires it first).
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::arch::kShootdownIpiVector),
                      shootdown_ipi_stub, GDT_KERNEL_CODE, kIRQAttr, 2);

    // xHCI event-ring MSI-X interrupt (F5-M5 Batch 0C, vector kXhciIrqVector).
    // Registered at boot so the shared IDT has the entry before APs start.  The
    // handler is a no-op+counter until Batch 2C wires the controller, and MSI-X
    // is not programmed until then, so it never fires prematurely.
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::drivers::usb::kXhciIrqVector),
                      xhci_irq_stub, GDT_KERNEL_CODE, kIRQAttr, 2);

    // NVMe MSI-X interrupt (F5-M3 batch 4, vector kNvmeIrqVector=0x41). Registered
    // at boot so the shared IDT has the entry before APs start; MSI-X is not
    // enabled until init_msi_x, so it never fires prematurely.  IST 2 -- same as
    // PIT/xHCI/LAPIC timer.  F13-B (54f6392) flipped those four IRQ vectors to
    // IST 2 but missed the three MSI-X vectors (NVMe/VirtIO-blk/net); leaving
    // ist=0 lands the ISR's fxsave+frame on the interrupted task's stack, which
    // corrupts it under gcc/g++ loads (NVMe is the boot disk, so its IRQ fires
    // densely on every rootfs read) -> the same intermittent IST-family panic.
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::drivers::nvme::kNvmeIrqVector),
                      nvme_irq_stub, GDT_KERNEL_CODE, kIRQAttr, 2);

    // VirtIO-blk MSI-X interrupt (F5-M2 batch 3, vector kVirtioBlkIrqVector=0x42).
    // Registered at boot so the shared IDT has the entry before APs start; MSI-X
    // is not enabled until init_msi_x (production only), so it never fires
    // prematurely, and the test kernel never enables MSI-X.  IST 2 -- see NVMe
    // above (one of the three MSI-X vectors 54f6392 missed).
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::drivers::virtio::kVirtioBlkIrqVector),
                      virtio_blk_irq_stub, GDT_KERNEL_CODE, kIRQAttr, 2);

    // VirtIO-net MSI-X interrupt (F5-M2 batch 5, vector kVirtioNetIrqVector=0x43).
    // Single-vector mode (RX/TX share entry 0).  Registered at boot; MSI-X not
    // enabled until init_msi_x (production only).  IST 2 -- see NVMe above.
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::drivers::virtio::kVirtioNetIrqVector),
                      virtio_net_irq_stub, GDT_KERNEL_CODE, kIRQAttr, 2);

    // LAPIC timer (F5-M5 -smp, vector kLapicTimerVector).  Registered into the
    // shared IDT so APs can take it; the BSP is preempted by the PIT and never
    // arms its LAPIC timer, so this stays dormant there.  See ap_main().
    g_idt.set_handler(static_cast<ExceptionVector>(cinux::arch::kLapicTimerVector),
                      lapic_timer_stub, GDT_KERNEL_CODE, kIRQAttr, 2);

    kprintf("[IRQ] All IRQ handlers registered.\n");
}
