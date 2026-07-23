/**
 * @file kernel/drivers/tty/console_tty.cpp
 * @brief System console TTY singleton + blocking stdin (F10-M3 batch 2/3)
 */

#include "kernel/drivers/tty/tty.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::drivers {

// Static storage: the TTY ctor fills the default termios, so the instance is
// usable before console_tty_init() (just without an echo sink).
static TTY g_console_tty;

// The single task blocked in console_tty_read() waiting for a line.  Single
// reader (the shell) is the real-world shape; multi-reader stdin is a
// follow-up.  Touched under IRQ-off on the reading CPU (InterruptGuard in the
// reader, IRQ context in the feeder), so single-CPU is race-free; cross-CPU
// (SMP) needs the Mutex-style spinlock treatment as a later follow-up.
static cinux::proc::Task* g_reader = nullptr;

TTY& console_tty() {
    return g_console_tty;
}

// Echo through kprintf so typed chars reach the same sink as stdout (serial +
// the registered Console).  kprintf/Console::putc are lock-free, so calling
// this from the keyboard IRQ context cannot deadlock; a rare re-entrant
// overlap only risks a transient cursor glitch (deferred-echo is a follow-up).
static void echo_via_kprintf(char c, void* /*ctx*/) {
    cinux::lib::kprintf("%c", c);
}

void console_tty_init() {
    Termios tm;
    make_default_termios(tm);
    tm.c_cc[kVerase] = 0x08;  // keyboard driver emits ^H for the Backspace key
    g_console_tty.set_termios(tm);
    g_console_tty.set_echo_sink(echo_via_kprintf, nullptr);
}

size_t console_tty_read(char* buf, size_t len) {
    auto* self = cinux::proc::Scheduler::current();
    for (;;) {
        {
            // IRQs off: the check + register-as-reader + mark-Blocked must be
            // atomic vs the keyboard feeder, so a line arriving in the window is
            // seen (and a feeder's wakeup finds us already Blocked -> no lost
            // wakeup, per the F3 prepare_to_wait design).
            cinux::proc::InterruptGuard guard;
            size_t                      n = g_console_tty.read_cooked(buf, len);
            if (n > 0) {
                return n;
            }
            if (g_console_tty.take_eof()) {
                return 0;  // Ctrl+D on an empty line -> EOF
            }
            g_reader = self;
            cinux::proc::Scheduler::prepare_to_wait(self);
        }  // IRQs restored BEFORE switching out
        cinux::proc::Scheduler::schedule_blocked();
        // Woken by the feeder (g_reader cleared); loop and re-read.
    }
}

void console_tty_input(char c) {
    InputResult r = g_console_tty.input_char(c);
    if (r == InputResult::kLineReady || r == InputResult::kEof) {
        auto* w = g_reader;
        if (w != nullptr) {
            g_reader = nullptr;
            cinux::proc::Scheduler::unblock(w);
        }
    }
}

}  // namespace cinux::drivers
