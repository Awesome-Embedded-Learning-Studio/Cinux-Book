/**
 * @file kernel/drivers/keyboard/usb_keyboard.cpp
 * @brief UsbKeyboard: HID boot-keyboard bring-up + report decode over an xHCI slot
 *
 * Collects the keyboard-specific concerns: SET_PROTOCOL (HID class request), the
 * report buffer, and the boot-keyboard decode.  Transport (control transfer,
 * Configure Endpoint, interrupt-IN async poll) is delegated to XhciSlot.  Mirrors
 * drivers/mouse/usb_mouse.cpp.
 *
 * Namespace: cinux::drivers
 */

#include "usb_keyboard.hpp"

#include <stdint.h>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/drivers/usb/usb_descriptor.hpp"
#include "kernel/drivers/usb/usb_request.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"
#include "kernel/drivers/usb/xhci_slot.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::drivers {
using namespace cinux::drivers::usb;

namespace {
constexpr uint64_t kReportBytes = 8;  ///< boot-keyboard report (modifier + reserved + 6 keycodes)

void zero_bytes(void* p, uint64_t n) {
    auto* b = static_cast<uint8_t*>(p);
    for (uint64_t i = 0; i < n; ++i) {
        b[i] = 0;
    }
}
}  // namespace

cinux::lib::ErrorOr<void> UsbKeyboard::init(XHCIController& hc, const BootKeyboardEp& ep) {
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
    // first async transfer separately (arm()).
    return slot_->add_interrupt_endpoint(hc, ep.ep_number, ep.max_packet_size, ep.interval);
}

void UsbKeyboard::on_transfer_complete(uint8_t /*slot_id*/, uint8_t /*epid*/, const Trb& ev) {
    // Runs in hard-IRQ context (the xHCI MSI-X handler).  The controller has
    // written the 8-byte report into report_buf_; decode it and inject into the
    // Keyboard event queue (edge-detected press/release), then re-arm.
    const uint32_t code = cmd_completion_code(ev.status);
    if (code == CompCode::kSuccess || code == CompCode::kShortPacket) {
        const HidKeyboardReport rpt =
            decode_boot_keyboard(static_cast<const uint8_t*>(report_buf_.virt()));
        Keyboard::inject_usb_report(rpt.modifier, rpt.keycodes, 6);
    } else {
        cinux::lib::kprintf("[xHCI] keyboard transfer error: completion code=%u\n", code);
    }

    arm();  // re-arm: submit the next async IN transfer
}

void UsbKeyboard::arm() {
    if (slot_ != nullptr && hc_ != nullptr) {
        slot_->submit_interrupt_in_async(*hc_, report_buf_.phys(), report_len_);
    }
}

}  // namespace cinux::drivers
