#include "kernel/console/console.hpp"

#include "cinux/spin_mutex.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/serial/serial.hpp"

namespace cinux::console {

namespace {
base::SpinMutex g_console_gate;

void put_char(char character) {
    driver::SerialPutChar(character);
    TextConsole::self().put_char(character);
}
}

void InitConsole(const cinux::boot::BootInfo& info) {
    cinux::driver::SerialInit();
    cinux::console::TextConsole::self().init(info.framebuffer);
}

void PutChar(char character) {
    const arch::IrqGuard kIrq;
    const auto           kLock = g_console_gate.guard();
    put_char(character);
}

void PutString(const char* text, bool newline) {
    const arch::IrqGuard kIrq;
    const auto           kLock = g_console_gate.guard();
    while (*text != '\0') {
        put_char(*text++);
    }
    if (newline) {
        put_char('\n');
    }
}

void Flush() {
    const arch::IrqGuard kIrq;
    const auto           kLock = g_console_gate.guard();
    cinux::driver::SerialDrain();
}

}  // namespace cinux::console
