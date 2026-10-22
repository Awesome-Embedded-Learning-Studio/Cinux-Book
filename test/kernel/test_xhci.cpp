#include "framework_kernel.hpp"
#include "kernel/driver/usb/usb_config.hpp"
#include "kernel/driver/usb/usb_keyboard.hpp"
#include "kernel/driver/usb/xhci.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner)

namespace {

TEST("xhci: the controller brings up and a keyboard answers") {
    auto& host = cinux::driver::XhciHost::self();
    ASSERT_TRUE(host.init());
    ASSERT_TRUE(host.present());
    ASSERT_GT(host.max_ports(), 0U);

    auto& keyboard = cinux::driver::UsbKeyboard::self();
    ASSERT_TRUE(keyboard.init());
    ASSERT_GT(keyboard.seated_slot(), 0U);
    ASSERT_LE(keyboard.seated_slot(), cinux::driver::usb::kXhciSlotsEnabled);
}

}  // namespace
