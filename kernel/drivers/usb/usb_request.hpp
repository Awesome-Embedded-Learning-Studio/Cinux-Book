/**
 * @file kernel/drivers/usb/usb_request.hpp
 * @brief USB standard control-request encoding (the 8-byte SETUP packet)
 *
 * Every USB control transfer begins with an 8-byte SETUP packet.  This header
 * encodes the bmRequestType direction/type/recipient bits, the standard
 * device-request codes, and a little-endian 8-byte pack consumed verbatim by the
 * xHCI Setup Stage TRB parameter field.  Pure types -- host-compilable, no
 * hardware.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::usb {

// ============================================================
// bmRequestType fields (byte 0 of SETUP)
// bit 7 = direction, [6:5] = type, [4:0] = recipient
// ============================================================

namespace UsbDir {
constexpr uint8_t kOut = 0;  ///< Host -> device
constexpr uint8_t kIn  = 1;  ///< Device -> host
}  // namespace UsbDir

namespace UsbReqType {
constexpr uint8_t kStandard = 0;  ///< USB standard request
constexpr uint8_t kClass    = 1;  ///< Class-specific (e.g. HID SET_PROTOCOL)
constexpr uint8_t kVendor   = 2;  ///< Vendor-specific
}  // namespace UsbReqType

namespace UsbRecipient {
constexpr uint8_t kDevice    = 0;
constexpr uint8_t kInterface = 1;
constexpr uint8_t kEndpoint  = 2;
constexpr uint8_t kOther     = 3;
}  // namespace UsbRecipient

// ============================================================
// Standard device requests (byte 1 of SETUP, bRequest)
// ============================================================

namespace UsbRequest {
constexpr uint8_t kGetStatus        = 0;
constexpr uint8_t kClearFeature     = 1;
constexpr uint8_t kSetFeature       = 3;
/// Not issued over the bus by xHCI: the controller assigns the device address
/// via the Address Device command.  Defined for protocol completeness.
constexpr uint8_t kSetAddress       = 5;
constexpr uint8_t kGetDescriptor    = 6;
constexpr uint8_t kSetDescriptor    = 7;
constexpr uint8_t kGetConfiguration = 8;
constexpr uint8_t kSetConfiguration = 9;
constexpr uint8_t kGetInterface     = 10;
constexpr uint8_t kSetInterface     = 11;
constexpr uint8_t kSynchFrame       = 12;
}  // namespace UsbRequest

/// Encode bmRequestType from its three fields.
constexpr uint8_t bm_request_type(uint8_t dir, uint8_t type, uint8_t recipient) {
    return static_cast<uint8_t>((dir << 7) | (type << 5) | (recipient & 0x1F));
}

// ============================================================
// 8-byte SETUP packet (consumed inline by the xHCI Setup Stage TRB)
// ============================================================

/// USB SETUP packet.  The uint16 fields are little-endian on x86_64, matching
/// the USB wire order; the host fills, the device reads as-is.
struct UsbSetup {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};
static_assert(sizeof(UsbSetup) == 8, "SETUP packet must be 8 bytes");

/// Pack the 8-byte SETUP packet into a little-endian uint64_t for the xHCI Setup
/// Stage TRB parameter field (Immediate Data -- the packet is copied inline).
constexpr uint64_t setup_to_u64(const UsbSetup& s) {
    return static_cast<uint64_t>(s.bmRequestType) | (static_cast<uint64_t>(s.bRequest) << 8) |
           (static_cast<uint64_t>(s.wValue) << 16) | (static_cast<uint64_t>(s.wIndex) << 32) |
           (static_cast<uint64_t>(s.wLength) << 48);
}

/// Build a SETUP packet from its raw fields.
constexpr UsbSetup make_setup(uint8_t bm_request_type, uint8_t b_request, uint16_t w_value,
                              uint16_t w_index, uint16_t w_length) {
    return UsbSetup{bm_request_type, b_request, w_value, w_index, w_length};
}

/// Convenience: GET_DESCRIPTOR (always IN / standard / device).  @p desc_type is
/// a UsbDescType value (usb_descriptor.hpp); wValue = (type << 8) | index.
constexpr UsbSetup make_get_descriptor_setup(uint8_t desc_type, uint16_t index, uint16_t length) {
    return make_setup(
        bm_request_type(UsbDir::kIn, UsbReqType::kStandard, UsbRecipient::kDevice),
        UsbRequest::kGetDescriptor,
        static_cast<uint16_t>((static_cast<uint16_t>(desc_type) << 8) | (index & 0xFF)), 0, length);
}

}  // namespace cinux::drivers::usb
