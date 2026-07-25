/**
 * @file kernel/drivers/usb/hid_boot.hpp
 * @brief USB HID boot-protocol interface + interrupt-IN endpoint location.
 *
 * Walks a GET_DESCRIPTOR(Configuration) blob to find a HID boot interface
 * (class 0x03 / subclass 0x01 / protocol = mouse|keyboard) and its interrupt-IN
 * endpoint.  Formerly duplicated as find_boot_mouse / find_boot_keyboard in
 * drivers/mouse/hid.hpp + drivers/keyboard/hid.hpp -- identical except the one
 * protocol byte.  Pure (host-testable, no hardware).
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

#include "kernel/drivers/usb/usb_descriptor.hpp"

namespace cinux::drivers::usb {

/// A located HID boot interface + its interrupt-IN endpoint.  Mouse and
/// keyboard share this on-wire shape (only the interface protocol byte differs,
/// which the caller passes to find_boot_hid).
struct BootHidEp {
    uint8_t  interface_number;  ///< HID interface (for SET_PROTOCOL wIndex)
    uint8_t  ep_number;         ///< endpoint number (low nibble of bEndpointAddress)
    uint16_t max_packet_size;   ///< wMaxPacketSize (boot HID interrupt = 8)
    uint8_t  interval;          ///< bInterval (poll period)
};

/// Walk a config-descriptor blob to find the HID boot interface whose protocol
/// matches @p protocol (UsbHid::kBootProtoMouse / kBootProtoKeyboard) and its
/// interrupt-IN endpoint.  Descriptors are length-prefixed (bLength at byte 0).
/// @return true and fills @p out on success.
inline bool find_boot_hid(const uint8_t* desc, uint32_t len, uint8_t protocol, BootHidEp& out) {
    uint32_t pos       = 0;
    bool     in_iface  = false;
    uint8_t  iface_num = 0;
    while (pos + 2 <= len) {
        const uint8_t dlen  = desc[pos];
        const uint8_t dtype = desc[pos + 1];
        if (dlen < 2) {
            break;  // malformed -- stop
        }

        if (dtype == UsbDescType::kInterface && dlen >= 9 && pos + 9 <= len) {
            // UsbInterfaceDescriptor: [5]=class [6]=subclass [7]=protocol [2]=ifNumber
            in_iface = (desc[pos + 5] == UsbHid::kInterfaceClass &&
                        desc[pos + 6] == UsbHid::kBootSubclass && desc[pos + 7] == protocol);
            iface_num = desc[pos + 2];
        } else if (dtype == UsbDescType::kEndpoint && dlen >= 7 && pos + 7 <= len) {
            if (in_iface) {
                // UsbEndpointDescriptor: [2]=addr [3]=attrs [4,5]=maxpacket [6]=interval
                const uint8_t ep_addr = desc[pos + 2];
                const uint8_t ep_attr = desc[pos + 3];
                if ((ep_addr & 0x80) != 0 && (ep_attr & 0x03) == UsbXfer::kInterrupt) {
                    out.interface_number = iface_num;
                    out.ep_number        = static_cast<uint8_t>(ep_addr & 0x0F);
                    out.max_packet_size  = static_cast<uint16_t>(desc[pos + 4]) |
                                           (static_cast<uint16_t>(desc[pos + 5]) << 8);
                    out.interval         = desc[pos + 6];
                    return true;
                }
            }
        }

        pos += dlen;
    }
    return false;
}

}  // namespace cinux::drivers::usb
