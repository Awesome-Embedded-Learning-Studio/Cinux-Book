/**
 * @file kernel/drivers/mouse/hid.hpp
 * @brief USB HID boot-protocol decode + configuration-descriptor walk
 *
 * Pure helpers (host-testable, no hardware): decode a HID boot mouse report
 * (3-4 bytes) into logical movement, and walk a GET_DESCRIPTOR(Configuration)
 * blob to locate the HID boot-mouse interface + its interrupt-IN endpoint.
 * The xHCI interrupt-ring poll that fetches the report lives in XhciSlot
 * (Batch 4A); the boot wiring + event-queue injection lands in 4B/5A.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

#include "kernel/drivers/usb/usb_descriptor.hpp"

namespace cinux::drivers::usb {

// UsbXfer (transfer type) now lives in usb_descriptor.hpp -- shared with
// drivers/keyboard/hid.hpp without a cross-include.

// ============================================================
// HID boot mouse report decode
// ============================================================

/// HID boot-protocol mouse report (3-4 bytes).  Buttons: bit0=left,
/// bit1=right, bit2=middle.  dx/dy/wheel are signed two's-complement.
/// NOTE: HID Y+ is "away from the user" == screen DOWN, so the cursor's
/// screen_y advances by +dy (NOT inverted, unlike PS/2 mice).
struct HidMouseReport {
    uint8_t buttons;
    int8_t  dx;
    int8_t  dy;
    int8_t  wheel;
};

/// Decode a 3-or-4-byte boot mouse report.  @p r points to >=3 bytes; the
/// optional 4th byte (wheel) is read if present (caller guarantees the buffer
/// holds the report).  Pure.
constexpr HidMouseReport decode_boot_mouse(const uint8_t* r) {
    return HidMouseReport{r[0], static_cast<int8_t>(r[1]), static_cast<int8_t>(r[2]),
                          static_cast<int8_t>(r[3])};
}

// ============================================================
// HID tablet (absolute pointer) report decode -- QEMU usb-tablet
// ============================================================

/// HID absolute-pointer report (QEMU usb-tablet, 5 bytes): byte0 = buttons
/// (bit0=left / 1=right / 2=middle), then X and Y as 16-bit little-endian in
/// the logical range 0..32767 (0x7FFF).  Unlike the boot mouse, X/Y are
/// ABSOLUTE screen coordinates, so the guest cursor can track the host cursor
/// exactly (no two-cursor edge drift).
struct TabletReport {
    uint8_t  buttons;  ///< bit0=left, bit1=right, bit2=middle
    uint16_t x;        ///< absolute X, 0..32767
    uint16_t y;        ///< absolute Y, 0..32767
};

/// Decode a 5-byte QEMU usb-tablet report.  @p r points to >=5 bytes.  Pure.
constexpr TabletReport decode_tablet(const uint8_t* r) {
    return TabletReport{static_cast<uint8_t>(r[0] & 0x07),
                        static_cast<uint16_t>(static_cast<uint16_t>(r[1]) |
                                              (static_cast<uint16_t>(r[2]) << 8)),
                        static_cast<uint16_t>(static_cast<uint16_t>(r[3]) |
                                              (static_cast<uint16_t>(r[4]) << 8))};
}

// ============================================================
// Configuration-descriptor walk -> HID boot mouse interrupt-IN endpoint
// ============================================================

/// The interrupt-IN endpoint of an enumerated HID boot mouse.
struct BootMouseEp {
    uint8_t  interface_number;  ///< HID interface (for SET_PROTOCOL wIndex)
    uint8_t  ep_number;         ///< endpoint number (low nibble of bEndpointAddress)
    uint16_t max_packet_size;   ///< wMaxPacketSize (boot mouse interrupt = 8)
    uint8_t  interval;          ///< bInterval (poll period)
};

/// Walk a GET_DESCRIPTOR(Configuration) blob to find the HID boot-mouse
/// interface (class 0x03 / subclass 0x01 / protocol 0x02) and its interrupt-IN
/// endpoint.  Descriptors are length-prefixed (bLength at byte 0).  Returns true
/// and fills @p out on success.  Pure (host-testable).
inline bool find_boot_mouse(const uint8_t* desc, uint32_t len, BootMouseEp& out) {
    uint32_t pos             = 0;
    bool     in_mouse_iface  = false;
    uint8_t  mouse_iface_num = 0;
    while (pos + 2 <= len) {
        const uint8_t dlen  = desc[pos];
        const uint8_t dtype = desc[pos + 1];
        if (dlen < 2) {
            break;  // malformed -- stop
        }

        if (dtype == UsbDescType::kInterface && dlen >= 9 && pos + 9 <= len) {
            // UsbInterfaceDescriptor: [5]=class [6]=subclass [7]=protocol [2]=ifNumber
            const uint8_t ifclass    = desc[pos + 5];
            const uint8_t ifsubclass = desc[pos + 6];
            const uint8_t ifproto    = desc[pos + 7];
            in_mouse_iface =
                (ifclass == UsbHid::kInterfaceClass && ifsubclass == UsbHid::kBootSubclass &&
                 ifproto == UsbHid::kBootProtoMouse);
            mouse_iface_num = desc[pos + 2];
        } else if (dtype == UsbDescType::kEndpoint && dlen >= 7 && pos + 7 <= len) {
            if (in_mouse_iface) {
                // UsbEndpointDescriptor: [2]=addr [3]=attrs [4,5]=maxpacket [6]=interval
                const uint8_t ep_addr = desc[pos + 2];
                const uint8_t ep_attr = desc[pos + 3];
                const bool    ep_in   = (ep_addr & 0x80) != 0;
                const uint8_t ep_xfer = ep_attr & 0x03;
                if (ep_in && ep_xfer == UsbXfer::kInterrupt) {
                    out.interface_number = mouse_iface_num;
                    out.ep_number        = ep_addr & 0x0F;
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
