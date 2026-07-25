/**
 * @file kernel/drivers/usb/usb_init.hpp
 * @brief Boot-time USB bring-up: xHCI discovery + HID boot mouse async input
 *
 * Batch 5A.  Wires the xHCI driver into the boot path: discovers the controller
 * via PCI, brings it up, enumerates the first HID boot mouse, and arms its
 * async interrupt-IN input path.  Reports arrive via the MSI-X event-ring
 * interrupt (Batch 2C) -> poll_events -> the mouse's TransferListener
 * (UsbMouse::on_transfer_complete), which decodes + injects into the GUI event
 * queue and re-arms -- no worker thread, zero CPU while the mouse is idle.
 *
 * Call after switch_to_apic + sti and before the scheduler runs (input is
 * interrupt-driven).  Graceful no-op if no xHCI controller is present, so the
 * baseline run-kernel-test (no qemu-xhci) stays green.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

namespace cinux::drivers::usb {

/// Bring up USB input at boot (see file header).  Idempotent in effect: finds
/// at most one xHCI controller + one boot mouse.
void init();

/// Service the xHCI event ring once.  Called each frame from the GUI worker:
/// dequeues transfer events -> device listeners -> decode + inject + re-arm.
/// Cheap when idle (dequeue finds the ring empty).  No-op if no controller was
/// enumerated (e.g. run-kernel-test's QEMU has no qemu-xhci).
///
/// On QEMU under nested-KVM the MSI-X transfer-complete interrupt is not
/// reliably latched, so this poll is the production event-service path for
/// mouse/keyboard reports.  When USB is compiled out, a no-op stub of the same
/// name is linked instead (CMake file gate) -- callers need no #ifdef.
void poll_input();

}  // namespace cinux::drivers::usb
