target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/pit/pit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/keyboard/keyboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/framebuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/font_data.S
    ${CMAKE_CURRENT_SOURCE_DIR}/video/console.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tty/tty.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tty/console_tty.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pci/pci.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/acpi/rsdp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/acpi/sdt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/acpi/madt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/apic/local_apic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/apic/io_apic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ahci/ahci.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dma/dma_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ahci/ahci_block_device.cpp
)

if(CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/canvas.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mouse/mouse.cpp
    )
else()
    # §14: mouse.cpp is GUI-only (PS/2 mouse feeds the GUI event queue).  Non-GUI
    # builds link mouse_stub.cpp so irq12_stub's reference to mouse_irq12_handler
    # resolves (no #ifndef in irq_handlers.cpp).
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mouse/mouse_stub.cpp
    )
endif()

# F5-M5 xHCI: USB split into core transport + HID injection (both §14 file gates).
# Core (xHCI controller/rings/slots + MSI-X): if(CINUX_USB).  It uses the
# TransferListener base class (xhci_controller.hpp) and does NOT depend on
# Mouse/Keyboard, so it builds without GUI.
# HID (usb_init + usb_mouse/usb_tablet/usb_keyboard): depends on Mouse/Keyboard
# (GUI-gated), so gated on (CINUX_USB AND CINUX_GUI).
if(CINUX_USB)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pci/msix.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pci/msix_controller.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_irq.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_controller.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_ring.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_slot.cpp
    )
endif()

if(CINUX_USB AND CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usb_init.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mouse/usb_mouse.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mouse/usb_tablet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/keyboard/usb_keyboard.cpp
    )
endif()

# usb::init()/poll_input() stub: when HID is off (no USB, OR no GUI), so init.cpp
# calls resolve with no #ifdef at the call site.  USB core above still links under
# CINUX_USB, but usb::init is a no-op, so the controller is never brought up --
# harmless dead code when GUI is off.
if(NOT (CINUX_USB AND CINUX_GUI))
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usb_stub.cpp
    )
endif()

# xhci_irq_handler stub: when CINUX_USB is off (xhci_irq.cpp absent).  USB builds
# link the real handler in xhci_irq.cpp; this only fills the asm stub's reference.
if(NOT CINUX_USB)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usb_xhci_stub.cpp
    )
endif()

# F5-M6 e1000: Intel NIC driver (core controller), §14 file gate like USB.
# Independent of GUI/USB (standalone PCI hardware).  批a: detect + reset +
# EEPROM MAC; 批b adds the legacy RX ring + polled receive.  TX and the MSI
# interrupt path are deferred.
if(CINUX_NET)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/net/e1000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/net/e1000_init.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/net/net_init.cpp
    )
else()
    # net::init() stub: when CINUX_NET is off (e1000 driver absent), main.cpp's
    # call resolves with no #ifdef at the call site (§14 file gate).
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/net/net_stub.cpp
    )
endif()
