/**
 * @file kernel/drivers/usb/usb_stub.cpp
 * @brief No-op stubs for the USB boot API when CINUX_USB is off
 *
 * CODING-TASTE §14: the USB subsystem is a CMake file gate, not a source-level
 * #ifdef.  When CINUX_USB is off, usb_init.cpp (and the whole xHCI driver) is
 * not compiled; this file supplies empty definitions of usb::init() /
 * usb::poll_input() so callers (kernel/proc/init.cpp) link without any #ifdef.
 * Exactly one of {usb_init.cpp, usb_stub.cpp} is selected by
 * drivers/CMakeLists.txt.
 *
 * Namespace: cinux::drivers::usb
 */

#include "kernel/drivers/usb/usb_init.hpp"

namespace cinux::drivers::usb {

void init() {
    // USB compiled out -- nothing to bring up.
}

void poll_input() {
    // USB compiled out -- no event ring to service.
}

}  // namespace cinux::drivers::usb
