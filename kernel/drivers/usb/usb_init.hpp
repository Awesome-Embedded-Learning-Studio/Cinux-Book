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

}  // namespace cinux::drivers::usb
