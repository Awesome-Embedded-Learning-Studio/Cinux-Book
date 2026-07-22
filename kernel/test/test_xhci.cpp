/**
 * @file kernel/test/test_xhci.cpp
 * @brief QEMU in-kernel integration tests for the xHCI controller (F5-M5)
 *
 * Runs inside the big kernel test suite.  PCI-enumerates an xHCI controller
 * (class 0x0C / subclass 0x03 / prog_if 0x30); if none is present (the default
 * QEMU config has no qemu-xhci) the test SKIPS (no failure).  Under the
 * run-kernel-test-xhci target (-device qemu-xhci ...) it exercises the real
 * bring-up: PCI find + BAR0 map + halt/reset, asserting MaxPorts > 0 and the
 * post-reset halted/CNR-clear state.
 *
 * Preconditions: VMM initialised (g_vmm.init in main_test.cpp) for BAR0 map.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/mouse/hid.hpp"
#ifdef CINUX_GUI
#    include "kernel/drivers/mouse/usb_mouse.hpp"
#endif
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/usb/usb_descriptor.hpp"
#include "kernel/drivers/usb/xhci_controller.hpp"
#include "kernel/drivers/usb/xhci_slot.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::drivers::pci::PCIDevice;
using cinux::drivers::pci::PCI;
// Usbsts is a namespace, which a using-declaration cannot name (GCC) -- use a
// using-directive for the usb namespace instead.  Same GOTCHA as test_msix.
using namespace cinux::drivers::usb;

// ============================================================
// Test 1: find xHCI + bring up (skip if no controller present)
// ============================================================

namespace test_xhci {

void test_find_and_reset() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_xhci(dev)) {
        cinux::lib::kprintf("[xHCI] no controller present -- skipping reset test\n");
        return;  // default QEMU config has no qemu-xhci: counts as a pass
    }

    XHCIController            xhci;
    cinux::lib::ErrorOr<void> r = xhci.init(dev);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_GT(static_cast<unsigned>(xhci.max_ports()), 0u);

    // Post-reset: controller halted (HCH set), CNR clear.
    const uint32_t sts = xhci.op_regs()->usbsts;
    TEST_ASSERT_TRUE((sts & Usbsts::kHcHalted) != 0);
    TEST_ASSERT_FALSE((sts & Usbsts::kControllerNotReady) != 0);

    // Bring up rings + DCBAA + interrupter and run (Batch 2B).
    cinux::lib::ErrorOr<void> rs = xhci.start();
    TEST_ASSERT_TRUE(rs.ok());
    const uint32_t run_sts = xhci.op_regs()->usbsts;
    TEST_ASSERT_FALSE((run_sts & Usbsts::kHcHalted) != 0);  // now running

    cinux::lib::kprintf("[xHCI] bring-up test passed: MaxPorts=%u run USBSTS=0x%x\n",
                        static_cast<unsigned>(xhci.max_ports()), run_sts);

    // Batch 2C: submit a NOOP command + ring doorbell 0.  The controller
    // executes it, writes a Command Completion Event to the event ring, and
    // raises the event interrupt (USBSTS.EINT).  The test kernel keeps CPU
    // interrupts off, so we poll the event ring directly and observe EINT;
    // live MSI-X -> CPU delivery (handler runs) is proven in the production
    // kernel (Batch 5A, which has sti + APIC).
    xhci.submit_command(0, 0, trb_control(TrbType::kNoOp));

    for (uint32_t i = 0; i < 1000000; ++i) {
        xhci.poll_events();
        if (xhci.cmd_completions() > 0) {
            break;
        }
    }
    const uint32_t usbsts = xhci.op_regs()->usbsts;
    cinux::lib::kprintf("[xHCI] command pipeline: cmd_completions=%u EINT=%d USBSTS=0x%x\n",
                        xhci.cmd_completions(),
                        static_cast<int>((usbsts & Usbsts::kEventInterrupt) != 0), usbsts);

    TEST_ASSERT_GT(xhci.cmd_completions(), 0u);  // Command Completion Event arrived
    // EINT (USBSTS bit 3) set => the controller asserted an event-ring interrupt.
    TEST_ASSERT_TRUE((usbsts & Usbsts::kEventInterrupt) != 0);
}

// ============================================================
// Test 2: Address Device -- enumerate one connected USB device
// (Batch 3B).  Runs only under run-kernel-test-xhci (a device is
// attached); skips if no controller or no connected port.
// ============================================================

void test_address_device() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_xhci(dev)) {
        cinux::lib::kprintf("[xHCI] no controller present -- skipping address-device test\n");
        return;
    }

    XHCIController xhci;
    if (!xhci.init(dev).ok()) {
        TEST_ASSERT_TRUE(false);
        return;
    }
    if (!xhci.start().ok()) {
        TEST_ASSERT_TRUE(false);
        return;
    }

    // Scan ports for a connected device (CCS).
    uint8_t port = 0xFF;
    for (uint8_t p = 0; p < xhci.max_ports(); ++p) {
        if (xhci.read_portsc(p) & Portsc::kCurrentConnect) {
            port = p;
            break;
        }
    }
    if (port == 0xFF) {
        cinux::lib::kprintf("[xHCI] no connected device -- skipping address-device test\n");
        return;  // no device attached: counts as a pass (skip)
    }

    // Reset the port; the controller negotiates and reports the speed.
    auto speed_r = xhci.port_reset(port);
    TEST_ASSERT_TRUE(speed_r.ok());
    const uint32_t speed = speed_r.value();

    // Enable Slot -> the controller returns the assigned slot ID.
    auto es = xhci.run_command(0, 0, trb_control(TrbType::kEnableSlot));
    TEST_ASSERT_TRUE(es.ok());
    const uint8_t slot_id = static_cast<uint8_t>(cmd_completion_slot_id(es.value().control));
    cinux::lib::kprintf("[xHCI] enable slot -> slot_id=%u (port=%u speed=%u)\n",
                        static_cast<unsigned>(slot_id), static_cast<unsigned>(port), speed);
    TEST_ASSERT_TRUE(slot_id >= 1);
    TEST_ASSERT_TRUE(slot_id <= xhci.max_slots());

    // Allocate contexts and register the device context in the DCBAA.
    XhciSlot slot;
    TEST_ASSERT_TRUE(slot.allocate(slot_id).ok());
    xhci.dcbaa_set(slot_id, slot.device_context_phys());

    // EP0 max packet: 64 for high speed, 8 otherwise (unknown pre-descriptor).
    const uint32_t ep0_maxpacket = (speed == UsbSpeed::kHigh) ? 64 : 8;
    slot.build_address_input(speed, port + 1, ep0_maxpacket);  // rh_port is 1-based

    // Address Device (BSR=0 -- complete the address stage).
    auto ad = xhci.run_command(slot.input_context_phys(), 0,
                               trb_control(TrbType::kAddressDevice) | slot_id_for_trb(slot_id));
    TEST_ASSERT_TRUE(ad.ok());
    const uint32_t code = cmd_completion_code(ad.value().status);
    cinux::lib::kprintf("[xHCI] address device -> completion code=%u\n", code);
    TEST_ASSERT_EQ(code, CompCode::kSuccess);

    // Verify the device entered the Addressed state (slot DW3 dev_state [31:27]).
    const uint32_t dev_state  = slot.device_slot_dword(3);
    const uint32_t slot_state = (dev_state >> 27) & 0x1F;
    cinux::lib::kprintf("[xHCI] device slot_state=%u dev_addr=%u\n", slot_state, dev_state & 0xFF);
    TEST_ASSERT_EQ(slot_state, SlotState::kAddressed);

    // Batch 3C: GET_DESCRIPTOR(Device) -- read the 18-byte device descriptor
    // via a 3-stage control IN transfer on EP0.
    auto gd = slot.get_descriptor(xhci, UsbDescType::kDevice, 0, sizeof(UsbDeviceDescriptor));
    TEST_ASSERT_TRUE(gd.ok());
    TEST_ASSERT_EQ(gd.value(), sizeof(UsbDeviceDescriptor));
    const auto* dd = reinterpret_cast<const UsbDeviceDescriptor*>(slot.data_virt());
    cinux::lib::kprintf(
        "[xHCI] device descriptor: bLength=%u vid=0x%x pid=0x%x class=0x%x ncfg=%u\n",
        static_cast<unsigned>(dd->bLength), static_cast<unsigned>(dd->idVendor),
        static_cast<unsigned>(dd->idProduct), static_cast<unsigned>(dd->bDeviceClass),
        static_cast<unsigned>(dd->bNumConfigurations));
    TEST_ASSERT_EQ(static_cast<unsigned>(dd->bLength), sizeof(UsbDeviceDescriptor));
    TEST_ASSERT_EQ(static_cast<unsigned>(dd->bDescriptorType), UsbDescType::kDevice);

    // Batch 3C: SET_CONFIGURATION(1) -- configure the device (2-stage, no data).
    auto sc = slot.set_configuration(xhci, 1);
    TEST_ASSERT_TRUE(sc.ok());
    cinux::lib::kprintf("[xHCI] set_configuration(1) ok\n");
}

// ============================================================
// Test 3: HID boot mouse -- scan ports for a boot-mouse device,
// configure its interrupt-IN endpoint (Batch 4A).  Skips if no
// controller / no mouse.  The report poll is best-effort: an idle
// mouse NAKs (no Transfer Event) -- that is correct, not a failure.
// ============================================================

// HID mouse bring-up needs UsbMouse (compiled only under CINUX_USB AND CINUX_GUI,
// see drivers/CMakeLists.txt).  Gate the whole test + its RUN_TEST on CINUX_GUI
// so a USB=ON+GUI=OFF build keeps the xHCI core tests without an unresolved
// UsbMouse vtable.
#ifdef CINUX_GUI
void test_hid_mouse() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_xhci(dev)) {
        cinux::lib::kprintf("[xHCI] no controller present -- skipping HID mouse test\n");
        return;
    }

    XHCIController xhci;
    if (!xhci.init(dev).ok() || !xhci.start().ok()) {
        TEST_ASSERT_TRUE(false);
        return;
    }

    // Scan ports; address each connected device, read its config descriptor,
    // and boot the first HID boot mouse found.
    for (uint8_t port = 0; port < xhci.max_ports(); ++port) {
        if (!(xhci.read_portsc(port) & Portsc::kCurrentConnect)) {
            continue;
        }

        auto speed_r = xhci.port_reset(port);
        if (!speed_r.ok()) {
            continue;
        }
        auto es = xhci.run_command(0, 0, trb_control(TrbType::kEnableSlot));
        if (!es.ok()) {
            continue;
        }
        const uint8_t slot_id = static_cast<uint8_t>(cmd_completion_slot_id(es.value().control));
        XhciSlot      slot;
        if (!slot.allocate(slot_id).ok()) {
            continue;
        }
        xhci.dcbaa_set(slot_id, slot.device_context_phys());
        const uint32_t speed = speed_r.value();
        const uint32_t maxp  = (speed == UsbSpeed::kHigh) ? 64 : 8;
        slot.build_address_input(speed, port + 1, maxp);
        auto ad = xhci.run_command(slot.input_context_phys(), 0,
                                   trb_control(TrbType::kAddressDevice) | slot_id_for_trb(slot_id));
        if (!ad.ok() || cmd_completion_code(ad.value().status) != CompCode::kSuccess) {
            continue;  // not addressable -- try the next port
        }

        // Read the config descriptor + look for a HID boot mouse.
        auto cfg = slot.get_descriptor(xhci, UsbDescType::kConfiguration, 0, 255);
        if (!cfg.ok()) {
            continue;
        }
        BootMouseEp mep{};
        if (!find_boot_mouse(slot.data_virt(), cfg.value(), mep)) {
            continue;  // not a boot mouse (e.g. the keyboard) -- try the next port
        }

        // Found the mouse: configure the device, then boot the HID mouse via the
        // UsbMouse driver (SET_PROTOCOL + Configure Endpoint on the bound slot).
        TEST_ASSERT_TRUE(slot.set_configuration(xhci, 1).ok());
        cinux::drivers::UsbMouse mouse;
        mouse.bind(slot);
        auto mi = mouse.init(xhci, mep);
        cinux::lib::kprintf("[xHCI] HID mouse: iface=%u ep%u-IN maxp=%u interval=%u -> boot %s\n",
                            static_cast<unsigned>(mep.interface_number),
                            static_cast<unsigned>(mep.ep_number),
                            static_cast<unsigned>(mep.max_packet_size),
                            static_cast<unsigned>(mep.interval), mi.ok() ? "ok" : "FAILED");
        TEST_ASSERT_TRUE(mi.ok());

        // (Batch 5A) mouse.poll() was best-effort (no assertion) but its
        // run_transfer busy-waits ~1M iterations on an idle-mouse NAK -- too
        // slow under nested KVM, and obsolete now: the production input path is
        // async (usb_init arms an interrupt-IN transfer; reports arrive via the
        // MSI-X event-ring interrupt -> UsbMouse::on_transfer_complete).  The
        // init() success above already proves the HID boot mouse is configured.
        return;  // booted the mouse
    }

    cinux::lib::kprintf("[xHCI] no HID boot mouse found -- skipping HID test\n");
}
#endif

}  // namespace test_xhci

extern "C" void run_xhci_tests() {
    TEST_SECTION("xHCI");
    RUN_TEST(test_xhci::test_find_and_reset);
    RUN_TEST(test_xhci::test_address_device);
#ifdef CINUX_GUI
    RUN_TEST(test_xhci::test_hid_mouse);
#endif
    TEST_SUMMARY();
}
