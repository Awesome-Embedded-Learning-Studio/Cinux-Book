/**
 * @file kernel/drivers/mouse/mouse_stub.cpp
 * @brief IRQ12 handler stub when CINUX_GUI is off (§14 file gate)
 *
 * In non-GUI builds mouse.cpp is not compiled (drivers/CMakeLists.txt gates it
 * under if(CINUX_GUI)), so this file supplies an empty mouse_irq12_handler to
 * satisfy the asm irq12_stub's link reference (interrupts.S calls it with C
 * ABI).  EOI is sent by the ISR stub.  §14 pattern: one symbol, CMake selects
 * mouse.cpp (GUI) or mouse_stub.cpp (non-GUI).
 */

namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

extern "C" void mouse_irq12_handler(cinux::arch::InterruptFrame* /*frame*/) {
    // EOI is owned by the ISR stub (irq12_stub in interrupts.S).
}
