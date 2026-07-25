/**
 * @file kernel/drivers/usb/usb_init.cpp
 * @brief Boot USB bring-up: discover xHCI, enumerate HID boot mouse + keyboard
 *
 * Batch 5A wired the mouse; 5B adds the keyboard.  The enumeration sequence
 * mirrors kernel/test/test_xhci.cpp (port scan -> reset -> Enable Slot ->
 * Address Device -> GET_DESCRIPTOR(Configuration) -> find_boot_mouse /
 * find_boot_keyboard -> Configure Endpoint), but targets the production async
 * input path: each device registers as a TransferListener and arms its first
 * async interrupt-IN transfer.  From then on input is interrupt-driven.
 *
 * Namespace: cinux::drivers::usb
 */

#include "usb_init.hpp"

#include <stdint.h>

#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/drivers/keyboard/usb_keyboard.hpp"
#include "kernel/drivers/mouse/mouse.hpp"
#include "kernel/drivers/mouse/usb_tablet.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/usb/usb_descriptor.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"
#include "kernel/drivers/usb/xhci_slot.hpp"
#include "kernel/lib/kprintf.hpp"

// Boot-device HID descriptor walks (find_boot_mouse / find_boot_keyboard) +
// their endpoint structs live in the per-subsystem hid.hpp headers.
#include "kernel/drivers/keyboard/hid.hpp"
#include "kernel/drivers/mouse/hid.hpp"

namespace cinux::drivers::usb {

// Persistent storage for the controller + the enumerated devices.  These must
// outlive boot: the controller's rings/contexts and each slot's DMA buffers
// stay live for the system lifetime, and the mouse/keyboard are TransferListeners
// the ISR calls on every report.  One XhciSlot per device (mouse + keyboard).
namespace {
XHCIController              g_xhci;
XhciSlot                    g_slots[2];  ///< one per enumerated boot device
cinux::drivers::UsbTablet   g_tablet;
cinux::drivers::UsbKeyboard g_keyboard;
}  // namespace

/// Enumerate the device on @p port: reset, Enable Slot, Address Device, read the
/// config descriptor.  Fills @p slot (bound to slot_id) on success and returns
/// the descriptor length; returns 0 on any failure (caller tries the next port).
static uint32_t enumerate_port(uint8_t port, XhciSlot& slot) {
    if (!(g_xhci.read_portsc(port) & Portsc::kCurrentConnect)) {
        return 0;
    }
    auto speed_r = g_xhci.port_reset(port);
    if (!speed_r.ok()) {
        return 0;
    }
    auto es = g_xhci.run_command(0, 0, trb_control(TrbType::kEnableSlot));
    if (!es.ok()) {
        return 0;
    }
    const uint8_t slot_id = static_cast<uint8_t>(cmd_completion_slot_id(es.value().control));
    if (!slot.allocate(slot_id).ok()) {
        return 0;
    }
    g_xhci.dcbaa_set(slot_id, slot.device_context_phys());

    const uint32_t speed = speed_r.value();
    const uint32_t maxp  = (speed == UsbSpeed::kHigh) ? 64 : 8;
    slot.build_address_input(speed, port + 1, maxp);
    auto ad = g_xhci.run_command(slot.input_context_phys(), 0,
                                 trb_control(TrbType::kAddressDevice) | slot_id_for_trb(slot_id));
    if (!ad.ok() || cmd_completion_code(ad.value().status) != CompCode::kSuccess) {
        return 0;
    }

    auto cfg = slot.get_descriptor(g_xhci, UsbDescType::kConfiguration, 0, 255);
    if (!cfg.ok()) {
        return 0;
    }
    return cfg.value();
}

void init() {
    pci::PCI       pci;
    pci::PCIDevice dev{};
    if (!pci.find_xhci(dev)) {
        cinux::lib::kprintf("[xHCI] no controller present -- USB input disabled\n");
        return;  // default QEMU (run-kernel-test) has no qemu-xhci: graceful
    }

    if (!g_xhci.init(dev).ok() || !g_xhci.start().ok()) {
        cinux::lib::kprintf("[xHCI] controller bring-up failed -- USB input disabled\n");
        return;
    }
    XHCIController::set_instance(&g_xhci);

    bool     mouse_ok = false;
    bool     kbd_ok   = false;
    uint32_t slot_idx = 0;  // next free slot object (mouse + keyboard = 2)

    for (uint8_t port = 0; port < g_xhci.max_ports() && slot_idx < 2; ++port) {
        XhciSlot&      slot    = g_slots[slot_idx];
        const uint32_t cfg_len = enumerate_port(port, slot);
        if (cfg_len == 0) {
            continue;  // not connected / not addressable / no descriptor
        }
        const uint8_t* desc = slot.data_virt();

        BootMouseEp    mep{};
        BootKeyboardEp kep{};
        if (!mouse_ok && find_boot_hid(desc, cfg_len, UsbHid::kBootProtoMouse, mep)) {
            if (!slot.set_configuration(g_xhci, 1).ok()) {
                continue;
            }
            // The pointing device is a QEMU usb-tablet (absolute): find_boot_mouse
            // matches its HID boot-mouse interface (class 3/1/2); UsbTablet
            // decodes the absolute X/Y report so the cursor tracks the host.
            g_tablet.bind(slot);
            g_xhci.register_transfer_listener(slot.slot_id(), &g_tablet);
            if (!g_tablet.init(g_xhci, mep).ok()) {
                cinux::lib::kprintf("[xHCI] HID tablet init failed (port=%u)\n", port);
                continue;
            }
            g_tablet.arm();
            cinux::drivers::Mouse::set_usb_primary(true);
            mouse_ok = true;
            ++slot_idx;
            cinux::lib::kprintf("[xHCI] HID tablet armed: port=%u ep%u-IN (async absolute)\n", port,
                                mep.ep_number);
        } else if (!kbd_ok && find_boot_hid(desc, cfg_len, UsbHid::kBootProtoKeyboard, kep)) {
            if (!slot.set_configuration(g_xhci, 1).ok()) {
                continue;
            }
            g_keyboard.bind(slot);
            g_xhci.register_transfer_listener(slot.slot_id(), &g_keyboard);
            if (!g_keyboard.init(g_xhci, kep).ok()) {
                cinux::lib::kprintf("[xHCI] HID boot keyboard init failed (port=%u)\n", port);
                continue;
            }
            g_keyboard.arm();
            cinux::drivers::Keyboard::set_usb_primary(true);
            kbd_ok = true;
            ++slot_idx;
            cinux::lib::kprintf("[xHCI] HID boot keyboard armed: port=%u ep%u-IN (async)\n", port,
                                kep.ep_number);
        }
        // else: not a boot mouse/keyboard, or that device class is already bound
        // -- leave slot_idx so the next port reuses this slot object.
    }

    if (!mouse_ok && !kbd_ok) {
        cinux::lib::kprintf("[xHCI] no HID boot device found -- USB input disabled\n");
    } else {
        cinux::lib::kprintf("[xHCI] USB input primary (PS/2 standby)\n");
    }
}

void poll_input() {
    // Dequeue transfer events off the event ring -> device listeners (tablet /
    // keyboard) decode + inject + re-arm.  No-op until init() enumerated a
    // controller.  See header: this is the production event-service path under
    // nested-KVM where the MSI-X interrupt is not reliably latched.
    if (XHCIController::has_controller()) {
        XHCIController::instance().poll_events();
    }
}

}  // namespace cinux::drivers::usb
