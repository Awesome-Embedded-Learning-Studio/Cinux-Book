/**
 * @file kernel/drivers/usb/xhci_irq.hpp
 * @brief xHCI event-ring interrupt vector + dispatch hook
 *
 * The xHCI MSI-X interrupt lands on a fixed IDT vector (kXhciIrqVector,
 * registered in irq_init before APs boot).  The low-level ISR
 * (xhci_irq_stub in interrupts.S) calls xhci_irq_handler, which counts the
 * interrupt, runs an optional hook (set by the xHCI controller in Batch 2C to
 * service the event ring -- clear IMAN.IP, advance ERDP, unblock the worker),
 * and sends EOI.
 *
 * Batch 0C scope: vector + stub + handler + registration.  The hook is empty
 * (nullptr) until 2C wires the controller's event-ring service routine; the
 * "vector actually fires" proof is the 2C doorbell-NOOP test.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers::usb {

/// Fixed IDT vector for the xHCI event-ring MSI-X interrupt.  Chosen in the
/// free range (avoids 0x20-0x2F IRQs, 0x80 sigreturn, 0xE0 resched IPI,
/// 0xFF LAPIC spurious).
constexpr uint8_t kXhciIrqVector = 0x40;

/// Hook invoked from xhci_irq_handler to service the event ring.  Set by the
/// xHCI controller in Batch 2C; nullptr until then.
using XhciIrqHook = void (*)();

/// Set the event-ring service hook (called from ISR context after counting).
void set_xhci_irq_hook(XhciIrqHook hook);

/// Monotonic count of xHCI interrupts taken.  The Batch 2C doorbell-NOOP test
/// asserts this rises.
extern volatile uint64_t g_xhci_irq_count;

}  // namespace cinux::drivers::usb
