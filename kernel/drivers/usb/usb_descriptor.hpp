/**
 * @file kernel/drivers/usb/usb_descriptor.hpp
 * @brief USB descriptor layouts + HID boot constants
 *
 * Packed structs matching the USB 2.0 descriptor layouts read from a device via
 * GET_DESCRIPTOR.  Fields are little-endian on x86_64 (matching the wire order);
 * a big-endian host would need byte-swapping.  Sizes are pinned by static_assert
 * so a layout regression fails at compile time.  Pure types -- host-compilable.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::usb {

// ============================================================
// Descriptor type codes (bDescriptorType; high byte of SETUP wValue)
// ============================================================

namespace UsbDescType {
constexpr uint8_t kDevice        = 1;
constexpr uint8_t kConfiguration = 2;
constexpr uint8_t kString        = 3;
constexpr uint8_t kInterface     = 4;
constexpr uint8_t kEndpoint      = 5;
constexpr uint8_t kHid           = 0x21;  ///< HID class descriptor
constexpr uint8_t kReport        = 0x22;  ///< HID report descriptor
constexpr uint8_t kPhysical      = 0x23;
}  // namespace UsbDescType

// ============================================================
// Device descriptor (18 bytes) -- read via GET_DESCRIPTOR(Device)
// ============================================================

struct __attribute__((packed)) UsbDeviceDescriptor {
    uint8_t  bLength;          ///< 18
    uint8_t  bDescriptorType;  ///< UsbDescType::kDevice
    uint16_t bcdUSB;           ///< e.g. 0x0200 = USB 2.0
    uint8_t  bDeviceClass;     ///< 0 = defined per-interface (HID does this)
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;  ///< EP0 max packet (8/16/32/64 for FS/LS)
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;  ///< string index, 0 = none
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};
static_assert(sizeof(UsbDeviceDescriptor) == 18, "Device descriptor must be 18 bytes");

// ============================================================
// Configuration descriptor (9 bytes) -- head of the config blob
// ============================================================

struct __attribute__((packed)) UsbConfigDescriptor {
    uint8_t  bLength;          ///< 9
    uint8_t  bDescriptorType;  ///< UsbDescType::kConfiguration
    uint16_t wTotalLength;     ///< total config blob length (incl. iface + ep)
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;  ///< value passed to SET_CONFIGURATION
    uint8_t  iConfiguration;       ///< string index
    uint8_t  bmAttributes;         ///< bit 7 = self-powered, bit 6 = remote wakeup
    uint8_t  bMaxPower;            ///< units of 2 mA (USB 2.0)
};
static_assert(sizeof(UsbConfigDescriptor) == 9, "Config descriptor must be 9 bytes");

// ============================================================
// Interface descriptor (9 bytes)
// ============================================================

struct __attribute__((packed)) UsbInterfaceDescriptor {
    uint8_t bLength;          ///< 9
    uint8_t bDescriptorType;  ///< UsbDescType::kInterface
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;       ///< excluding EP0
    uint8_t bInterfaceClass;     ///< 0x03 = HID
    uint8_t bInterfaceSubClass;  ///< 0x01 = boot
    uint8_t bInterfaceProtocol;  ///< 0x01 = keyboard, 0x02 = mouse (boot)
    uint8_t iInterface;          ///< string index
};
static_assert(sizeof(UsbInterfaceDescriptor) == 9, "Interface descriptor must be 9 bytes");

// ============================================================
// Endpoint descriptor (7 bytes)
// ============================================================

struct __attribute__((packed)) UsbEndpointDescriptor {
    uint8_t  bLength;           ///< 7
    uint8_t  bDescriptorType;   ///< UsbDescType::kEndpoint
    uint8_t  bEndpointAddress;  ///< bit 7 = direction (1=IN), [3:0] = EP number
    uint8_t  bmAttributes;      ///< [1:0] = transfer type (0=ctl,1=iso,2=bulk,3=int)
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;  ///< polling interval (frames for interrupt EP)
};
static_assert(sizeof(UsbEndpointDescriptor) == 7, "Endpoint descriptor must be 7 bytes");

// ============================================================
// HID boot constants
// ============================================================

namespace UsbHid {
constexpr uint8_t kInterfaceClass    = 0x03;  ///< HID
constexpr uint8_t kBootSubclass      = 0x01;  ///< boot interface
constexpr uint8_t kBootProtoMouse    = 0x02;
constexpr uint8_t kBootProtoKeyboard = 0x01;
// HID class request codes (bRequest; bmRequestType = 0x21 OUT or 0xA1 IN).
constexpr uint8_t kSetProtocol       = 0x0B;  ///< wValue: 0=boot, 1=report
constexpr uint8_t kGetProtocol       = 0x03;
constexpr uint8_t kSetIdle           = 0x0A;
}  // namespace UsbHid

// ============================================================
// USB transfer type (endpoint bmAttributes [1:0]) -- shared by the mouse +
// keyboard HID descriptor walks (lives here so drivers/mouse/ and
// drivers/keyboard/ share it without cross-including each other).
// ============================================================

namespace UsbXfer {
constexpr uint8_t kControl     = 0;
constexpr uint8_t kIsochronous = 1;
constexpr uint8_t kBulk        = 2;
constexpr uint8_t kInterrupt   = 3;
}  // namespace UsbXfer

// ============================================================
// Endpoint address helpers
// ============================================================

/// Build bEndpointAddress: bit 7 = IN, [3:0] = endpoint number.
constexpr uint8_t ep_address(uint8_t number, bool in) {
    return static_cast<uint8_t>((number & 0x0F) | (in ? 0x80 : 0x00));
}

/// True if the endpoint direction is IN (device -> host).
constexpr bool ep_dir_in(uint8_t b_endpoint_address) {
    return (b_endpoint_address & 0x80) != 0;
}

}  // namespace cinux::drivers::usb
