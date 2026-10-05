#include "kernel/console/console.hpp"

#include "kernel/boot/boot_info.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/serial.hpp"

namespace cinux::console {

void InitConsole(const cinux::boot::BootInfo& info) {
    cinux::driver::SerialInit();
    cinux::console::TextConsole::self().init(info.framebuffer);
}

void PutChar(char character) {
    cinux::driver::SerialPutChar(character);
    cinux::console::TextConsole::self().put_char(character);
}

}  // namespace cinux::console
