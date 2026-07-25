/**
 * @file kernel/drivers/mouse/usb_mouse.hpp
 * @brief USB HID boot-mouse driver over an xHCI slot
 *
 * The HID-mouse APPLICATION layer: owns the report buffer + the mouse's
 * interrupt-IN endpoint configuration, polls the endpoint through the generic
 * XhciSlot transport (drivers/usb/), and decodes the boot-mouse report
 * (hid.hpp, co-located here).  Lives under drivers/mouse/ for input-subsystem
 * cohesion alongside the PS/2 Mouse sink; depends one-way on drivers/usb/ for
 * transport only.
 *
 * The decoded report is fed to Mouse::inject_usb_motion (dy NOT inverted).
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "hid.hpp"
#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"  // TransferListener -- UsbMouse implements it

namespace cinux::drivers::usb {
class XhciSlot;  // pointer member only
}  // namespace cinux::drivers::usb

namespace cinux::drivers {

/// USB HID boot mouse bound to one xHCI slot.  Owns its report buffer + the
/// interrupt-IN endpoint config; uses XhciSlot for transport + hid.hpp for
/// decode.  A worker polls poll() and forwards the report to Mouse.
class UsbMouse : public usb::TransferListener {
public:
    /// Bind to the xHCI slot carrying this mouse's USB device.
    void bind(usb::XhciSlot& slot) { slot_ = &slot; }

    /// Bring up the HID boot mouse: allocate the report buffer, SET_PROTOCOL
    /// (boot) on the interface, and add the interrupt-IN endpoint (Configure
    /// Endpoint).  @p ep from find_boot_mouse() over the config descriptor.
    /// Retains @p hc for the async re-arm path.
    cinux::lib::ErrorOr<void> init(usb::XHCIController& hc, const usb::BootMouseEp& ep);

    /// Poll once (test path): fetch a report from the interrupt-IN EP and decode
    /// it.  Returns Error::TimedOut when the mouse is idle (NAK) -- correct
    /// interrupt-IN behaviour, not an error.
    cinux::lib::ErrorOr<usb::HidMouseReport> poll(usb::XHCIController& hc);

    /// TransferListener (Batch 5A): a report arrived asynchronously (hard-IRQ
    /// context).  Decode it, inject into the Mouse event queue, and re-arm the
    /// next async IN transfer so input keeps flowing.
    void on_transfer_complete(uint8_t slot_id, uint8_t epid, const usb::Trb& ev) override;

    /// Submit the first async interrupt-IN transfer (call once after init(),
    /// before interrupts drive on_transfer_complete).  Production only -- the
    /// test kernel polls synchronously via poll() and must NOT arm (a pending
    /// async transfer would stall the synchronous poll path).
    void arm();

private:
    usb::XhciSlot*                 slot_ = nullptr;
    usb::XHCIController*           hc_   = nullptr;  ///< re-arm target (on_transfer_complete)
    cinux::drivers::dma::DmaBuffer report_buf_;      ///< DMA report buffer (controller writes here)
    uint32_t                       report_len_ = 0;  ///< bytes requested per poll
};

}  // namespace cinux::drivers
