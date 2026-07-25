/**
 * @file kernel/drivers/mouse/usb_mouse.cpp
 * @brief UsbMouse: HID boot-mouse bring-up + report poll over an xHCI slot
 *
 * Collects the mouse-specific concerns that previously leaked into XhciSlot:
 * SET_PROTOCOL (HID class request), the report buffer, and the boot-mouse
 * decode.  Transport (control transfer, Configure Endpoint, interrupt-IN poll)
 * is delegated to XhciSlot.
 *
 * Namespace: cinux::drivers
 */

#include "usb_mouse.hpp"

#include <stdint.h>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/drivers/mouse/mouse.hpp"
#include "kernel/drivers/usb/usb_descriptor.hpp"
#include "kernel/drivers/usb/usb_request.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"
#include "kernel/drivers/usb/xhci_slot.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::drivers {
using namespace cinux::drivers::usb;

namespace {
constexpr uint64_t kReportBytes = 8;  ///< boot-mouse report (<= max_packet, typically 4)

void zero_bytes(void* p, uint64_t n) {
    auto* b = static_cast<uint8_t*>(p);
    for (uint64_t i = 0; i < n; ++i) {
        b[i] = 0;
    }
}
}  // namespace

cinux::lib::ErrorOr<void> UsbMouse::init(XHCIController& hc, const BootMouseEp& ep) {
    hc_     = &hc;  // retained for re-arm in on_transfer_complete
    auto rb = dma::g_dma_pool.alloc(kReportBytes);
    if (!rb.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    report_buf_ = std::move(rb.value());
    zero_bytes(report_buf_.virt(), kReportBytes);
    report_len_ = static_cast<uint32_t>(kReportBytes);

    // SET_PROTOCOL(boot) on the HID interface (class control transfer on EP0).
    const UsbSetup sp =
        make_setup(bm_request_type(UsbDir::kOut, UsbReqType::kClass, UsbRecipient::kInterface),
                   UsbHid::kSetProtocol, /*protocol=*/0, ep.interface_number, 0);
    auto r = slot_->control_out_no_data(hc, sp);
    if (!r.ok()) {
        return r.error();
    }

    // Add the interrupt-IN endpoint via Configure Endpoint.  The caller arms the
    // first async transfer separately (arm()) -- init() stays usable by the test
    // kernel, which polls synchronously via poll() instead.
    return slot_->add_interrupt_endpoint(hc, ep.ep_number, ep.max_packet_size, ep.interval);
}

cinux::lib::ErrorOr<HidMouseReport> UsbMouse::poll(XHCIController& hc) {
    zero_bytes(report_buf_.virt(), kReportBytes);
    auto n = slot_->poll_interrupt_in(hc, report_buf_.phys(), report_len_);
    if (!n.ok()) {
        return n.error();  // TimedOut = idle mouse NAK
    }
    return decode_boot_mouse(static_cast<const uint8_t*>(report_buf_.virt()));
}

void UsbMouse::on_transfer_complete(uint8_t /*slot_id*/, uint8_t /*epid*/, const Trb& ev) {
    // Reached from poll_events(), which the gui_worker calls each frame (the
    // MSI-X transfer-complete interrupt is not reliably delivered under
    // QEMU/nested-KVM, so the event ring is polled).  The controller has written
    // the report into report_buf_; decode it and inject into the GUI event queue
    // (Mouse::inject_usb_motion -- dy NOT inverted, HID convention).
    const uint32_t code = cmd_completion_code(ev.status);
    if (code == CompCode::kSuccess || code == CompCode::kShortPacket) {
        const HidMouseReport rpt =
            decode_boot_mouse(static_cast<const uint8_t*>(report_buf_.virt()));
        Mouse::inject_usb_motion(rpt.dx, rpt.dy, rpt.buttons);
    } else {
        cinux::lib::kprintf("[xHCI] mouse transfer error: completion code=%u\n", code);
    }

    // Re-arm: submit the next async IN transfer.  An idle mouse NAKs, so this
    // transfer stays pending (no event, no CPU) until the next report.
    arm();
}

void UsbMouse::arm() {
    if (slot_ != nullptr && hc_ != nullptr) {
        slot_->submit_interrupt_in_async(*hc_, report_buf_.phys(), report_len_);
    }
}

}  // namespace cinux::drivers
