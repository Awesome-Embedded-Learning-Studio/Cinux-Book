/**
 * @file kernel/drivers/keyboard/usb_keyboard.hpp
 * @brief USB HID boot-keyboard driver over an xHCI slot
 *
 * The HID-keyboard APPLICATION layer: owns the report buffer + the keyboard's
 * interrupt-IN endpoint configuration, submits the endpoint through the generic
 * XhciSlot transport (drivers/usb/), and decodes the boot-keyboard report
 * (hid.hpp, co-located).  Lives under drivers/keyboard/ for input-subsystem
 * cohesion alongside the PS/2 Keyboard sink; depends one-way on drivers/usb/
 * for transport only.  Mirrors drivers/mouse/usb_mouse.hpp.
 *
 * The decoded report is fed to Keyboard::inject_usb_report.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "hid.hpp"
#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"  // TransferListener -- UsbKeyboard implements it

namespace cinux::drivers::usb {
class XhciSlot;  // pointer member only
}  // namespace cinux::drivers::usb

namespace cinux::drivers {

/// USB HID boot keyboard bound to one xHCI slot.  Owns its report buffer + the
/// interrupt-IN endpoint config; uses XhciSlot for transport + hid.hpp for
/// decode.  Implements TransferListener: on_transfer_complete decodes the report,
/// injects it into the Keyboard event queue, and re-arms.
class UsbKeyboard : public usb::TransferListener {
public:
    /// Bind to the xHCI slot carrying this keyboard's USB device.
    void bind(usb::XhciSlot& slot) { slot_ = &slot; }

    /// Bring up the HID boot keyboard: allocate the report buffer, SET_PROTOCOL
    /// (boot) on the interface, and add the interrupt-IN endpoint (Configure
    /// Endpoint).  @p ep from find_boot_keyboard() over the config descriptor.
    /// Retains @p hc for the async re-arm path.
    cinux::lib::ErrorOr<void> init(usb::XHCIController& hc, const usb::BootKeyboardEp& ep);

    /// TransferListener (Batch 5B): a report arrived asynchronously (hard-IRQ
    /// context).  Decode it, inject into the Keyboard event queue, and re-arm
    /// the next async IN transfer.
    void on_transfer_complete(uint8_t slot_id, uint8_t epid, const usb::Trb& ev) override;

    /// Submit the first async interrupt-IN transfer (call once after init(),
    /// before interrupts drive on_transfer_complete).  Production only.
    void arm();

private:
    usb::XhciSlot*                 slot_ = nullptr;
    usb::XHCIController*           hc_   = nullptr;  ///< re-arm target (on_transfer_complete)
    cinux::drivers::dma::DmaBuffer report_buf_;      ///< DMA report buffer (8 bytes)
    uint32_t                       report_len_ = 0;  ///< bytes requested per report
};

}  // namespace cinux::drivers
