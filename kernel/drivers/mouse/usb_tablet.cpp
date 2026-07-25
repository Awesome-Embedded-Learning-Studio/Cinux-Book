/**
 * @file kernel/drivers/mouse/usb_tablet.cpp
 * @brief UsbTablet: HID absolute-pointer bring-up + report poll over an xHCI slot
 *
 * Sibling of UsbMouse for an absolute pointing device (QEMU usb-tablet).  Owns
 * the report buffer + interrupt-IN endpoint; transport (Configure Endpoint,
 * async interrupt-IN poll) is delegated to XhciSlot.  Decodes the absolute
 * report and forwards it to Mouse::inject_usb_absolute.
 *
 * Namespace: cinux::drivers
 */

#include "usb_tablet.hpp"

#include <stdint.h>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/drivers/mouse/mouse.hpp"
#include "kernel/drivers/usb/usb_descriptor.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"
#include "kernel/drivers/usb/xhci_slot.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::drivers {
using namespace cinux::drivers::usb;

namespace {
constexpr uint64_t kReportBytes = 8;  ///< tablet report (5 used: buttons + X16 + Y16)

void zero_bytes(void* p, uint64_t n) {
    auto* b = static_cast<uint8_t*>(p);
    for (uint64_t i = 0; i < n; ++i) {
        b[i] = 0;
    }
}
}  // namespace

cinux::lib::ErrorOr<void> UsbTablet::init(XHCIController& hc, const BootMouseEp& ep) {
    hc_     = &hc;  // retained for re-arm in on_transfer_complete
    auto rb = dma::g_dma_pool.alloc(kReportBytes);
    if (!rb.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    report_buf_ = std::move(rb.value());
    zero_bytes(report_buf_.virt(), kReportBytes);
    report_len_ = static_cast<uint32_t>(kReportBytes);

    // No SET_PROTOCOL(boot): a tablet has no relative boot mode and defaults to
    // its report (absolute) protocol, which is what we decode.  Just add the
    // interrupt-IN endpoint (Configure Endpoint).  The caller arms the first
    // async transfer separately (arm()).
    return slot_->add_interrupt_endpoint(hc, ep.ep_number, ep.max_packet_size, ep.interval);
}

void UsbTablet::on_transfer_complete(uint8_t /*slot_id*/, uint8_t /*epid*/, const Trb& ev) {
    // Reached from poll_events() (gui_worker each frame; the MSI-X interrupt is
    // not reliably delivered under QEMU/nested-KVM).  The controller wrote the
    // report into report_buf_; decode the absolute X/Y + buttons and set the
    // cursor directly so it tracks the host cursor.
    const uint32_t code = cmd_completion_code(ev.status);
    if (code == CompCode::kSuccess || code == CompCode::kShortPacket) {
        const TabletReport rpt = decode_tablet(static_cast<const uint8_t*>(report_buf_.virt()));
        Mouse::inject_usb_absolute(rpt.x, rpt.y, rpt.buttons);
    } else {
        cinux::lib::kprintf("[xHCI] tablet transfer error: completion code=%u\n", code);
    }

    // Re-arm: submit the next async IN transfer.  An idle tablet NAKs, so this
    // transfer stays pending (no event, no CPU) until the next report.
    arm();
}

void UsbTablet::arm() {
    if (slot_ != nullptr && hc_ != nullptr) {
        slot_->submit_interrupt_in_async(*hc_, report_buf_.phys(), report_len_);
    }
}

}  // namespace cinux::drivers
