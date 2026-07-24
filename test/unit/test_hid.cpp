/**
 * @file test/unit/test_hid.cpp
 * @brief Host unit tests for HID boot-mouse decode + config-descriptor walk
 *
 * Verifies the boot-mouse report decode (buttons/dx/dy/wheel) and the
 * configuration-descriptor walk that locates the HID boot-mouse interrupt-IN
 * endpoint.  Header-only -- no kernel sources linked.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

#    include "drivers/mouse/hid.hpp"
#    include "drivers/usb/hid_boot.hpp"  // find_boot_hid (refactor merged find_boot_mouse/keyboard)

using namespace cinux::drivers::usb;

// ============================================================
// 1. Boot-mouse report decode
// ============================================================

TEST("hid: decode_boot_mouse unpacks buttons + signed dx/dy/wheel") {
    uint8_t r[4]           = {0x05, 0x02, 0xFB, 0x01};  // buttons=left|middle, dx=2, dy=-5, wheel=1
    const HidMouseReport m = decode_boot_mouse(r);
    ASSERT_EQ(m.buttons, 0x05u);
    ASSERT_EQ(m.dx, 2);
    ASSERT_EQ(m.dy, -5);  // 0xFB signed = -5
    ASSERT_EQ(m.wheel, 1);
}

TEST("hid: decode_boot_mouse right button bit") {
    uint8_t r[4] = {0x02, 0x00, 0x00, 0x00};  // right button only
    ASSERT_EQ(decode_boot_mouse(r).buttons, 0x02u);
}

// ============================================================
// 2. Configuration-descriptor walk -> boot-mouse interrupt-IN endpoint
// ============================================================

TEST("hid: find_boot_mouse locates the interrupt-IN endpoint") {
    // config(9) + interface(HID boot mouse,9) + endpoint(EP1 IN interrupt,7)
    static const uint8_t cfg[] = {
        // config: len type wTotalLength(25=0x19) nIface cfgVal iConfig bmAttr bMaxPower
        0x09,
        0x02,
        0x19,
        0x00,
        0x01,
        0x01,
        0x00,
        0xA0,
        0x32,
        // interface: len type ifNum alt nEPs class(03) subclass(01) proto(02 mouse) iIface
        0x09,
        0x04,
        0x00,
        0x00,
        0x01,
        0x03,
        0x01,
        0x02,
        0x00,
        // endpoint: len type addr(0x81=EP1 IN) attr(0x03 interrupt) maxpacket(8) interval(0x0A)
        0x07,
        0x05,
        0x81,
        0x03,
        0x08,
        0x00,
        0x0A,
    };
    BootMouseEp ep{};
    ASSERT_TRUE(find_boot_hid(cfg, sizeof(cfg), UsbHid::kBootProtoMouse, ep));
    ASSERT_EQ(static_cast<uint32_t>(ep.interface_number), 0u);
    ASSERT_EQ(static_cast<uint32_t>(ep.ep_number), 1u);
    ASSERT_EQ(ep.max_packet_size, 8u);
    ASSERT_EQ(static_cast<uint32_t>(ep.interval), 0x0Au);
}

TEST("hid: find_boot_mouse ignores a keyboard (proto 0x01) interface") {
    static const uint8_t cfg[] = {
        0x09, 0x02, 0x19, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,  // proto=01 keyboard
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
    };
    BootMouseEp ep{};
    ASSERT_FALSE(find_boot_hid(cfg, sizeof(cfg), UsbHid::kBootProtoMouse, ep));
}

TEST("hid: find_boot_mouse skips an OUT interrupt endpoint, takes the IN one") {
    // interface with EP1 OUT interrupt then EP2 IN interrupt
    static const uint8_t cfg[] = {
        0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x01, 0x02, 0x00,  // mouse, 2 EPs
        0x07, 0x05, 0x01, 0x03, 0x08, 0x00, 0x0A,              // EP1 OUT interrupt
        0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0A,              // EP2 IN interrupt
    };
    BootMouseEp ep{};
    ASSERT_TRUE(find_boot_hid(cfg, sizeof(cfg), UsbHid::kBootProtoMouse, ep));
    ASSERT_EQ(static_cast<uint32_t>(ep.ep_number), 2u);  // the IN endpoint
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
