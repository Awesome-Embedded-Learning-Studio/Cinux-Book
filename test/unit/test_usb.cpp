/**
 * @file test/unit/test_usb.cpp
 * @brief Host unit tests for USB SETUP packet + descriptor encoding
 *
 * Verifies bmRequestType bit packing, the standard request codes, SETUP packet
 * little-endian u64 packing (the xHCI Setup Stage TRB parameter), descriptor
 * sizes, and HID boot constants.  Header-only -- no kernel sources linked.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

#    include "drivers/usb/usb_descriptor.hpp"
#    include "drivers/usb/usb_request.hpp"

using namespace cinux::drivers::usb;

// ============================================================
// 1. bmRequestType bit packing (direction | type | recipient)
// ============================================================

TEST("usb: IN/Standard/Device request type is 0x80") {
    ASSERT_EQ(bm_request_type(UsbDir::kIn, UsbReqType::kStandard, UsbRecipient::kDevice), 0x80u);
}

TEST("usb: OUT/Standard/Device request type is 0x00") {
    ASSERT_EQ(bm_request_type(UsbDir::kOut, UsbReqType::kStandard, UsbRecipient::kDevice), 0x00u);
}

TEST("usb: IN/Class/Interface request type is 0xA1 (HID class request)") {
    // bit7=1 (IN) | [6:5]=01 (class)=0x20 | [4:0]=01 (interface) = 0xA1
    ASSERT_EQ(bm_request_type(UsbDir::kIn, UsbReqType::kClass, UsbRecipient::kInterface), 0xA1u);
}

// ============================================================
// 2. SETUP packet + little-endian u64 pack
// ============================================================

TEST("usb: UsbSetup is 8 bytes") {
    ASSERT_EQ(sizeof(UsbSetup), 8u);
}

TEST("usb: GET_DESCRIPTOR(Device) builds IN/standard/device, bRequest=6, wValue=0x0100") {
    const UsbSetup s = make_get_descriptor_setup(UsbDescType::kDevice, 0, 64);
    ASSERT_EQ(s.bmRequestType, 0x80u);
    ASSERT_EQ(s.bRequest, UsbRequest::kGetDescriptor);
    ASSERT_EQ(s.wValue, 0x0100u);  // (type << 8) | index
    ASSERT_EQ(s.wIndex, 0u);
    ASSERT_EQ(s.wLength, 64u);
}

TEST("usb: GET_DESCRIPTOR(Configuration) encodes type 0x02 in wValue high byte") {
    const UsbSetup s = make_get_descriptor_setup(UsbDescType::kConfiguration, 0, 255);
    ASSERT_EQ(s.wValue, 0x0200u);
}

TEST("usb: SET_CONFIGURATION builds OUT/standard/device, bRequest=9") {
    const UsbSetup s =
        make_setup(bm_request_type(UsbDir::kOut, UsbReqType::kStandard, UsbRecipient::kDevice),
                   UsbRequest::kSetConfiguration, 1, 0, 0);
    ASSERT_EQ(s.bmRequestType, 0x00u);
    ASSERT_EQ(s.bRequest, 0x09u);
    ASSERT_EQ(s.wValue, 1u);
}

TEST("usb: setup_to_u64 packs fields little-endian") {
    UsbSetup s{};
    s.bmRequestType         = 0x80;
    s.bRequest              = 0x06;
    s.wValue                = 0x0100;
    s.wIndex                = 0x0000;
    s.wLength               = 0x0040;
    // byte0=0x80 byte1=0x06 bytes2-3=0x0100 bytes4-5=0 bytes6-7=0x40
    const uint64_t expected = 0x80ULL | (0x06ULL << 8) | (0x0100ULL << 16) | (0x0040ULL << 48);
    ASSERT_EQ(setup_to_u64(s), expected);
}

// ============================================================
// 3. Descriptor sizes + HID boot constants + endpoint helpers
// ============================================================

TEST("usb: descriptor sizes match USB 2.0 (18/9/9/7)") {
    ASSERT_EQ(sizeof(UsbDeviceDescriptor), 18u);
    ASSERT_EQ(sizeof(UsbConfigDescriptor), 9u);
    ASSERT_EQ(sizeof(UsbInterfaceDescriptor), 9u);
    ASSERT_EQ(sizeof(UsbEndpointDescriptor), 7u);
}

TEST("usb: HID boot constants (class 0x03, subclass 0x01, mouse 0x02/kbd 0x01)") {
    ASSERT_EQ(UsbHid::kInterfaceClass, 0x03u);
    ASSERT_EQ(UsbHid::kBootSubclass, 0x01u);
    ASSERT_EQ(UsbHid::kBootProtoMouse, 0x02u);
    ASSERT_EQ(UsbHid::kBootProtoKeyboard, 0x01u);
}

TEST("usb: ep_address sets IN bit (0x80) and keeps number in low nibble") {
    ASSERT_EQ(ep_address(1, true), 0x81u);   // EP1 IN
    ASSERT_EQ(ep_address(2, false), 0x02u);  // EP2 OUT
}

TEST("usb: ep_dir_in reads bit 7") {
    ASSERT_TRUE(ep_dir_in(0x81));
    ASSERT_FALSE(ep_dir_in(0x02));
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
