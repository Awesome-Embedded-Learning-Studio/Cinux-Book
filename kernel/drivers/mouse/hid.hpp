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

#include "kernel/drivers/usb/hid_boot.hpp"
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

/// HID boot-mouse endpoint.  Same on-wire shape as every boot HID endpoint; the
/// struct + the config-descriptor walk live in kernel/drivers/usb/hid_boot.hpp
/// (BootHidEp + find_boot_hid, parameterised by protocol, shared with keyboard).
/// Alias kept so mouse-subsystem call sites read as "mouse".
using BootMouseEp = BootHidEp;

}  // namespace cinux::drivers::usb
