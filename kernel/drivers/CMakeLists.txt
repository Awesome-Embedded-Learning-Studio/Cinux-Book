target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/pit/pit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/keyboard/keyboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/framebuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/font_data.S
    ${CMAKE_CURRENT_SOURCE_DIR}/video/console.cpp
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
endif()

# F5-M5 xHCI: USB host controller + reusable PCI MSI-X subsystem.
# MSI-X (pci/msix.cpp) is a generic PCI capability helper; the xHCI driver
# (usb/*.cpp) is added in later batches under the same gate.
if(CINUX_USB)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pci/msix.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pci/msix_controller.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_irq.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_controller.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_ring.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/xhci_slot.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usb_init.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mouse/usb_mouse.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mouse/usb_tablet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/keyboard/usb_keyboard.cpp
    )
endif()
