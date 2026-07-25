/**
 * @file kernel/drivers/mouse/usb_tablet.hpp
 * @brief USB HID absolute-pointer (tablet) driver over an xHCI slot
 *
 * Sibling of UsbMouse for an ABSOLUTE pointing device (QEMU usb-tablet).  The
 * tablet reports absolute X/Y (0..32767) instead of relative dx/dy, so the
 * guest cursor is set directly to the host cursor's position -- the two
 * cursors coincide and there is no edge-clamp drift (the two-cursor skew a
 * relative usb-mouse suffers in a VM).  Transport is the same generic XhciSlot
 * path; only the report decode + the Mouse sink differ (inject_usb_absolute).
 *
 * The interface/endpoint is located with find_boot_mouse(): QEMU's usb-tablet
 * presents the same HID boot-mouse interface (class 3 / subclass 1 / protocol
 * 2), so the descriptor walk is shared.  No SET_PROTOCOL(boot) is sent -- the
 * tablet has no relative boot mode and defaults to its report (absolute)
 * protocol.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "hid.hpp"
#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"  // TransferListener -- UsbTablet implements it

namespace cinux::drivers::usb {
class XhciSlot;  // pointer member only
}  // namespace cinux::drivers::usb

namespace cinux::drivers {

/// USB HID absolute pointer (tablet) bound to one xHCI slot.  Owns its report
/// buffer + interrupt-IN endpoint; decodes the absolute report (hid.hpp) and
/// forwards it to Mouse::inject_usb_absolute.
class UsbTablet : public usb::TransferListener {
public:
    /// Bind to the xHCI slot carrying this tablet's USB device.
    void bind(usb::XhciSlot& slot) { slot_ = &slot; }

    /// Bring up the tablet: allocate the report buffer and add the interrupt-IN
    /// endpoint (Configure Endpoint).  No SET_PROTOCOL (the tablet defaults to
    /// its absolute report protocol).  @p ep from find_boot_mouse() over the
    /// config descriptor (shared with the boot mouse -- same interface class).
    /// Retains @p hc for the async re-arm path.
    cinux::lib::ErrorOr<void> init(usb::XHCIController& hc, const usb::BootMouseEp& ep);

    /// TransferListener: a report arrived asynchronously.  Decode the absolute
    /// report, inject into the Mouse event queue, and re-arm the next async IN
    /// transfer so input keeps flowing.
    void on_transfer_complete(uint8_t slot_id, uint8_t epid, const usb::Trb& ev) override;

    /// Submit the first async interrupt-IN transfer (call once after init()).
    void arm();

private:
    usb::XhciSlot*                 slot_ = nullptr;
    usb::XHCIController*           hc_   = nullptr;  ///< re-arm target (on_transfer_complete)
    cinux::drivers::dma::DmaBuffer report_buf_;      ///< DMA report buffer (controller writes here)
    uint32_t                       report_len_ = 0;  ///< bytes requested per report
};

}  // namespace cinux::drivers
