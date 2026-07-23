/**
 * @file kernel/drivers/tty/console_tty.cpp
 * @brief System console TTY: singleton, blocking stdin, signal delivery (F10-M3)
 */

#include "kernel/drivers/tty/console_tty.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/process.hpp"  // Task::pgid
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"  // killpg (interrupt/quit/suspend -> foreground pgrp)
#include "kernel/proc/sync.hpp"

namespace cinux::drivers {

namespace {
ConsoleTty g_console_tty;
}  // namespace

ConsoleTty& console_tty() {
    return g_console_tty;
}

void ConsoleTty::echo_via_kprintf(char c, void* /*ctx*/) {
    // kprintf/Console::putc are lock-free, so calling this from the keyboard
    // IRQ context cannot deadlock; a rare re-entrant overlap only risks a
    // transient cursor glitch (deferred-echo is a follow-up).
    cinux::lib::kprintf("%c", c);
}

void ConsoleTty::init() {
    Termios tm;
    make_default_termios(tm);
    tm.c_cc[kVerase] = 0x08;  // keyboard driver emits ^H for the Backspace key
    tty_.set_termios(tm);
    tty_.set_echo_sink(&ConsoleTty::echo_via_kprintf, nullptr);
}

size_t ConsoleTty::read(char* buf, size_t len) {
    auto* self = cinux::proc::Scheduler::current();
    for (;;) {
        {
            // IRQs off: the check + register-as-reader + mark-Blocked must be
            // atomic vs the keyboard feeder, so a line arriving in the window is
            // seen (and a feeder's wakeup finds us already Blocked -> no lost
            // wakeup, per the F3 prepare_to_wait design).
            cinux::proc::InterruptGuard guard;
            size_t                      n = tty_.read_cooked(buf, len);
            if (n > 0) {
                return n;
            }
            if (tty_.take_eof()) {
                return 0;  // Ctrl+D on an empty line -> EOF
            }
            reader_ = self;
            cinux::proc::Scheduler::prepare_to_wait(self);
        }  // IRQs restored BEFORE switching out
        cinux::proc::Scheduler::schedule_blocked();
        // Woken by the feeder (reader_ cleared); loop and re-read.
    }
}

void ConsoleTty::input(char c) {
    InputResult r = tty_.input_char(c);

    if (r == InputResult::kSignal) {
        // A signal char (interrupt/quit/suspend) maps to a POSIX signal and is
        // delivered to the foreground process group via killpg.  When no
        // foreground group is set, fall back to the blocked reader's group.
        // killpg's registry lock uses irq_guard, so this is safe from the
        // keyboard IRQ context.
        TtySignal           ts  = tty_.take_signal();
        cinux::proc::Signal sig = cinux::proc::Signal::kSigint;
        switch (ts) {
        case TtySignal::kSigquit:
            sig = cinux::proc::Signal::kSigquit;
            break;
        case TtySignal::kSigtstp:
            sig = cinux::proc::Signal::kSigtstp;
            break;
        case TtySignal::kSigint:
            sig = cinux::proc::Signal::kSigint;
            break;
        default:
            return;  // kNone: spurious, nothing to deliver
        }
        int pgid = foreground_pgid_;
        if (pgid == 0 && reader_ != nullptr) {
            pgid = reader_->pgid;
        }
        if (pgid != 0) {
            cinux::proc::killpg(pgid, sig);
        }
        return;
    }

    if (r == InputResult::kLineReady || r == InputResult::kEof) {
        if (reader_ != nullptr) {
            auto* w = reader_;
            reader_ = nullptr;
            cinux::proc::Scheduler::unblock(w);
        }
    }
}

TTY& ConsoleTty::tty() {
    return tty_;
}

int ConsoleTty::foreground_pgid() const {
    return foreground_pgid_;
}

void ConsoleTty::set_foreground_pgid(int pgid) {
    foreground_pgid_ = pgid;
}

}  // namespace cinux::drivers
