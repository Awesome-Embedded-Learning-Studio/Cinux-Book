/**
 * @file kernel/drivers/usb/xhci_irq.cpp
 * @brief xHCI event-ring interrupt handler (count + hook + EOI)
 *
 * xhci_irq_handler is the C target of the xhci_irq_stub ISR (interrupts.S).
 * It counts the interrupt, runs the controller's event-ring service hook if
 * set (Batch 2C), and sends EOI.  Kernel-only -- tied to the asm stub and
 * LAPIC EOI, so not host-testable; the end-to-end proof is the Batch 2C
 * doorbell-NOOP -> Command Completion Event test.
 *
 * Namespace: cinux::drivers::usb
 */

#include "xhci_irq.hpp"

namespace cinux::drivers::usb {

volatile uint64_t g_xhci_irq_count = 0;

}  // namespace cinux::drivers::usb

namespace {
/// Event-ring service hook set by the xHCI controller (Batch 2C).  File-static:
/// only reachable via set_xhci_irq_hook() and the handler below.
cinux::drivers::usb::XhciIrqHook g_xhci_hook = nullptr;
}  // namespace

void cinux::drivers::usb::set_xhci_irq_hook(XhciIrqHook hook) {
    g_xhci_hook = hook;
}

extern "C" void xhci_irq_handler(cinux::arch::InterruptFrame* /*frame*/) {
    ++cinux::drivers::usb::g_xhci_irq_count;
    if (g_xhci_hook != nullptr) {
        g_xhci_hook();
    }
    // EOI is sent by the ISR_IRQ stub after this handler returns.
}
