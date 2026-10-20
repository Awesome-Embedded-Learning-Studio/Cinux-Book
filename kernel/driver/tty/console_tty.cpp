#include "kernel/driver/tty/console_tty.hpp"

#include "kernel/console/console.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/keyboard/keyboard.hpp"
#include "kernel/driver/serial/serial.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::driver {

namespace {

/// One pass over both input seats: the serial line first, because the
/// hosted world types there, then the PS/2 keyboard the QEMU monitor feeds.
bool console_take_key(void* /*context*/, char* out) {
    if (SerialTryGetChar(out)) {
        return true;
    }
    const char kMaybe = Keyboard::self().take(false);
    if (kMaybe != 0) {
        *out = kMaybe;
        return true;
    }
    return false;
}

bool console_has_key(void* /*context*/) {
    return SerialHasKey() || Keyboard::self().poll();
}

void console_echo(void* /*context*/, char key) {
    console::PutChar(key);
}

void console_wait(void* /*context*/) {
    proc::Scheduler::self().yield();
}

const TtyWiring kConsoleWiring{
    .context  = nullptr,
    .take_key = console_take_key,
    .has_key  = console_has_key,
    .echo     = console_echo,
    .wait     = console_wait,
};

/// Shapes the terminal's window from the screen, once it has one.
void shape_window(Tty& terminal) {
    const auto kRows    = static_cast<unsigned short>(console::TextConsole::self().rows());
    const auto kColumns = static_cast<unsigned short>(console::TextConsole::self().columns());
    terminal.set_window(TtyWindow{
        .rows     = kRows,
        .columns  = kColumns,
        .pixels_x = 0,
        .pixels_y = 0,
    });
}

}  // namespace

Tty& ConsoleTty() {
    static constinit Tty terminal{};
    if (!terminal.wired()) {
        terminal.wire(kConsoleWiring);
    }
    if (terminal.window().rows == 0) {
        shape_window(terminal);
    }
    return terminal;
}

}  // namespace cinux::driver
