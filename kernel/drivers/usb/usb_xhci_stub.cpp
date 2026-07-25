/**
 * @file kernel/drivers/usb/usb_xhci_stub.cpp
 * @brief xHCI IRQ handler stub when CINUX_USB is off (§14 file gate)
 *
 * With CINUX_USB off, usb/xhci_irq.cpp is absent, so this file supplies an
 * empty xhci_irq_handler to satisfy the asm xhci_irq_stub's link reference
 * (interrupts.S, C ABI).  MSI-X is never programmed without the driver, so it
 * never fires.  USB builds link the real handler in xhci_irq.cpp instead.
 */

namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

extern "C" void xhci_irq_handler(cinux::arch::InterruptFrame* /*frame*/) {
    // EOI is owned by the ISR stub.
}
