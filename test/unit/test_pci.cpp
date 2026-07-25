/**
 * @file test/unit/test_pci.cpp
 * @brief Host unit tests for PCI device-class matching
 *
 * Exercises the constexpr is_xhci_device() predicate (header-only, in
 * pci.hpp).  The key assertion is that prog_if is required: a class+subclass-
 * only test would wrongly match UHCI/OHCI/EHCI.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

#    include "drivers/pci/pci.hpp"

using cinux::drivers::pci::is_xhci_device;

// ============================================================
// xHCI positive match
// ============================================================

TEST("pci: is_xhci_device matches xHCI (0x0C/0x03/0x30)") {
    ASSERT_TRUE(is_xhci_device(0x0C, 0x03, 0x30));
}

// ============================================================
// prog_if is mandatory -- reject the other USB host controller flavours
// ============================================================

TEST("pci: is_xhci_device rejects UHCI (prog_if 0x00)") {
    ASSERT_FALSE(is_xhci_device(0x0C, 0x03, 0x00));
}

TEST("pci: is_xhci_device rejects OHCI (prog_if 0x10)") {
    ASSERT_FALSE(is_xhci_device(0x0C, 0x03, 0x10));
}

TEST("pci: is_xhci_device rejects EHCI (prog_if 0x20)") {
    ASSERT_FALSE(is_xhci_device(0x0C, 0x03, 0x20));
}

TEST("pci: is_xhci_device rejects USB device (prog_if 0xFE)") {
    ASSERT_FALSE(is_xhci_device(0x0C, 0x03, 0xFE));
}

// ============================================================
// Wrong class / subclass
// ============================================================

TEST("pci: is_xhci_device rejects AHCI (class 0x01)") {
    ASSERT_FALSE(is_xhci_device(0x01, 0x06, 0x30));
}

TEST("pci: is_xhci_device rejects non-serial-bus class") {
    ASSERT_FALSE(is_xhci_device(0x02, 0x03, 0x30));
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
