/**
 * @file kernel/drivers/keyboard/hid.hpp
 * @brief USB HID boot-protocol keyboard decode + config-descriptor walk
 *
 * Pure helpers (host-testable): decode an 8-byte HID boot keyboard report into
 * modifier + keycodes, and walk a GET_DESCRIPTOR(Configuration) blob to locate
 * the HID boot-keyboard interface + its interrupt-IN endpoint.  Mirrors
 * drivers/mouse/hid.hpp for the mouse; co-located under drivers/keyboard/ for
 * keyboard-subsystem cohesion.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

#include "kernel/drivers/usb/usb_descriptor.hpp"

namespace cinux::drivers::usb {

// ============================================================
// HID boot-keyboard modifier bitmask (report byte 0)
// ============================================================
namespace HidKbdMod {
constexpr uint8_t kLCtrl  = 0x01;
constexpr uint8_t kLShift = 0x02;
constexpr uint8_t kLAlt   = 0x04;
constexpr uint8_t kLGUI   = 0x08;
constexpr uint8_t kRCtrl  = 0x10;
constexpr uint8_t kRShift = 0x20;
constexpr uint8_t kRAlt   = 0x40;
constexpr uint8_t kRGUI   = 0x80;
}  // namespace HidKbdMod

/// HID boot-protocol keyboard report (8 bytes): modifier + reserved + up to 6
/// keycodes (Keyboard/Keypad usage IDs, page 0x07).  A 6-key rollover overflow
/// fills the keycode bytes with 0x01 (rollover error).
struct HidKeyboardReport {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycodes[6];
};

/// Decode an 8-byte boot keyboard report.  Pure.
constexpr HidKeyboardReport decode_boot_keyboard(const uint8_t* r) {
    return HidKeyboardReport{r[0], r[1], {r[2], r[3], r[4], r[5], r[6], r[7]}};
}

// ============================================================
// HID Usage ID (Keyboard/Keypad page 0x07) -> ASCII.  Two tables (unshifted /
// shifted), indexed by usage ID.  0 = non-printable.  Verified against the USB
// HID Usage Tables v1.21 (US keyboard layout).  Covers usage IDs 0..58 (the
// printable range; 57+ are CapsLock / F-keys / etc, non-printable).  27 = ESC,
// matching the PS/2 table convention in keyboard.cpp.
// ============================================================

constexpr char kHidUnshifted[59] = {
    0,    0,   0,    0,  // 0-3 (reserved/error)
    'a',  'b', 'c',  'd',  'e',  'f', 'g', 'h',  'i', 'j', 'k', 'l', 'm',
    'n',  'o', 'p',  'q',  'r',  's', 't', 'u',  'v', 'w', 'x', 'y', 'z',  // 4-29 (a-z)
    '1',  '2', '3',  '4',  '5',  '6', '7', '8',  '9', '0',                 // 30-39
    '\n', 27,  '\b', '\t', ' ',  // 40-44 Enter/Esc/BS/Tab/Space
    '-',  '=', '[',  ']',  '\\', 0,   ';', '\'', '`', ',', '.', '/',  // 45-56
    0,    0  // 57-58 CapsLock/F1 (non-printable)
};

constexpr char kHidShifted[59] = {0,   0,   0,   0,   'A',  'B', 'C',  'D',  'E', 'F', 'G', 'H',
                                  'I', 'J', 'K', 'L', 'M',  'N', 'O',  'P',  'Q', 'R', 'S', 'T',
                                  'U', 'V', 'W', 'X', 'Y',  'Z', '!',  '@',  '#', '$', '%', '^',
                                  '&', '*', '(', ')', '\n', 27,  '\b', '\t', ' ', '_', '+', '{',
                                  '}', '|', 0,   ':', '"',  '~', '<',  '>',  '?', 0,   0};

/// Number of entries in the HID keycode->ASCII tables (usage IDs 0..58).
constexpr uint8_t kHidKeymapSize = 59;

// ============================================================
// Configuration-descriptor walk -> HID boot-keyboard interrupt-IN endpoint
// ============================================================

/// The interrupt-IN endpoint of an enumerated HID boot keyboard.
struct BootKeyboardEp {
    uint8_t  interface_number;  ///< HID interface (for SET_PROTOCOL wIndex)
    uint8_t  ep_number;         ///< endpoint number (low nibble of bEndpointAddress)
    uint16_t max_packet_size;   ///< wMaxPacketSize (boot keyboard interrupt = 8)
    uint8_t  interval;          ///< bInterval (poll period)
};

/// Walk a GET_DESCRIPTOR(Configuration) blob to find the HID boot-keyboard
/// interface (class 0x03 / subclass 0x01 / protocol 0x01) and its interrupt-IN
/// endpoint.  Mirrors find_boot_mouse; only the protocol differs.  Pure.
inline bool find_boot_keyboard(const uint8_t* desc, uint32_t len, BootKeyboardEp& out) {
    uint32_t pos           = 0;
    bool     in_kbd_iface  = false;
    uint8_t  kbd_iface_num = 0;
    while (pos + 2 <= len) {
        const uint8_t dlen  = desc[pos];
        const uint8_t dtype = desc[pos + 1];
        if (dlen < 2) {
            break;  // malformed -- stop
        }

        if (dtype == UsbDescType::kInterface && dlen >= 9 && pos + 9 <= len) {
            const uint8_t ifclass    = desc[pos + 5];
            const uint8_t ifsubclass = desc[pos + 6];
            const uint8_t ifproto    = desc[pos + 7];
            in_kbd_iface =
                (ifclass == UsbHid::kInterfaceClass && ifsubclass == UsbHid::kBootSubclass &&
                 ifproto == UsbHid::kBootProtoKeyboard);
            kbd_iface_num = desc[pos + 2];
        } else if (dtype == UsbDescType::kEndpoint && dlen >= 7 && pos + 7 <= len) {
            if (in_kbd_iface) {
                const uint8_t ep_addr = desc[pos + 2];
                const uint8_t ep_attr = desc[pos + 3];
                const bool    ep_in   = (ep_addr & 0x80) != 0;
                const uint8_t ep_xfer = ep_attr & 0x03;
                if (ep_in && ep_xfer == UsbXfer::kInterrupt) {
                    out.interface_number = kbd_iface_num;
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
