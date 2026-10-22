/**
 * @file    usb_keyboard.cpp
 * @brief   The class driver: find the endpoint, translate the reports.
 *
 * Enumeration reads the descriptor chain over the device's control
 * face, and every report completion arrives through the typed
 * callback — the report handler is a static member of this class, so
 * the owner rides in the signature, not in a void word.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/usb/usb_keyboard.hpp"

#include <stdint.h>

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/usb/xhci.hpp"
#include "kernel/driver/usb/xhci_layout.hpp"
#include "kernel/fs/devfs/devfs.hpp"

namespace cinux::driver {

namespace {

base::Result<unsigned long> silent_read(void* /*context*/, void* /*sink*/,
                                        unsigned long /*count*/) {
    return 0;
}

base::Result<unsigned long> accept_write(void* /*context*/, const void* /*source*/,
                                         unsigned long count) {
    return count;
}

const fs::DeviceOps kKeyboardOps{
    .read      = silent_read,
    .write     = accept_write,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

/// @brief How many descriptor bytes the descriptor buffer keeps.
constexpr unsigned long kDescriptorBytes = 128;

}  // namespace

bool UsbKeyboard::init() {
    if (device_.slot() != 0) {
        return true;
    }
    XhciHost& host = XhciHost::self();
    if (!host.present() || !device_.address(host)) {
        return false;
    }
    unsigned char descriptor[kDescriptorBytes] = {};
    if (!device_.control_read(host, 0x0100, descriptor, 8) ||
        !device_.control_read(host, 0x0100, descriptor, 18)) {
        return false;
    }
    if (!device_.control_read(host, 0x0200, descriptor, 9)) {
        return false;
    }
    const unsigned long kConfigBytes = static_cast<unsigned long>(descriptor[2]) |
                                       (static_cast<unsigned long>(descriptor[3]) << 8U);
    if (kConfigBytes == 0 || kConfigBytes > kDescriptorBytes ||
        !device_.control_read(host, 0x0200, descriptor, static_cast<uint16_t>(kConfigBytes))) {
        return false;
    }
    usb::KeyboardEp keyboard{};
    if (!usb::FindKeyboardInterface(descriptor, kConfigBytes, keyboard)) {
        return false;
    }
    if (!device_.set_configuration(host, keyboard.config_value)) {
        return false;
    }
    if (!device_.configure_interrupt_in(host, keyboard.max_packet, keyboard.interval)) {
        return false;
    }
    device_.interrupt_read(host, &UsbKeyboard::report_arrived, this);
    print::Println("[kern] usb keyboard seated, config %u ep %X max %u interval %u",
                   keyboard.config_value, keyboard.address, keyboard.max_packet, keyboard.interval);
    return true;
}

uint8_t UsbKeyboard::seated_slot() const {
    return device_.slot();
}

void UsbKeyboard::report_arrived(UsbKeyboard& self, unsigned long status, unsigned long length) {
    if (status == usb::kCompletionSuccess && length == sizeof(self.previous_)) {
        const unsigned char* report = self.device_.report();
        const char           kGlyph = usb::HidReportKey(report, self.previous_);
        base::CopyBytes(self.previous_, report, sizeof(self.previous_));
        if (kGlyph != 0) {
            self.events_.push(kGlyph);
        }
    }
    self.device_.interrupt_read(XhciHost::self(), &UsbKeyboard::report_arrived, &self);
}

bool UsbKeyboard::poll() {
    XhciHost::self().drain_events();
    return events_.count() != 0;
}

char UsbKeyboard::take() {
    poll();
    char code = 0;
    events_.pop(code);
    return code;
}

bool SeatUsbKeyboard() {
    if (!UsbKeyboard::self().init()) {
        return false;
    }
    const base::Result<void> kSeat = DeviceModel::self().seat({
        .klass       = DeviceClass::kCharacter,
        .name        = "usbkbd",
        .number      = fs::MakeDeviceId(10, 0),
        .ops         = &kKeyboardOps,
        .bus         = "usb",
        .driver_name = "xhci-hid",
    });
    return kSeat.ok();
}

}  // namespace cinux::driver
