/**
 * @file kernel/drivers/tty/console_tty.hpp
 * @brief System console TTY: singleton owning the line discipline, blocking
 *        stdin, and job-control signal delivery (F10-M3).
 *
 * The single stdin the kernel shell reads from.  Keyboard bytes feed input()
 * (line discipline + echo + signal delivery); sys_read fd==0 drains read().
 * Owns the echo sink wiring (kprintf) + the VERASE tweak that matches the
 * keyboard driver's Backspace key, so tty.cpp itself stays Console/kprintf-free.
 */

#pragma once

#include "kernel/drivers/tty/tty.hpp"

namespace cinux::proc {
struct Task;  // the blocked stdin reader; full def in process.hpp
}

namespace cinux::drivers {

/// The system console TTY.  Owns its line discipline (TTY), the single blocked
/// stdin reader, and the foreground process group for signal delivery.  A
/// single global instance is returned by console_tty().
class ConsoleTty {
public:
    /// Wire the echo sink -> kprintf and set VERASE = ^H (the keyboard driver
    /// sends 0x08 for Backspace; the termios default is DEL 0x7F).  Call once
    /// after kprintf/Console are up and before keyboard interrupts fire.
    void init();

    /// Read a cooked line from stdin.  Blocks (sleeps the caller) until the
    /// line discipline commits a line or EOF (Ctrl+D on an empty line).
    /// Returns the byte count, or 0 on EOF.
    size_t read(char* buf, size_t len);

    /// Feed one keyboard byte: line discipline + echo + cooked buffer, and on
    /// a signal char (interrupt/quit/suspend) deliver the corresponding signal
    /// to the foreground process group.  Called from the keyboard IRQ handler;
    /// wakes a blocked reader when a line or EOF is committed.
    void input(char c);

    /// Underlying line discipline (for ioctl termios read/write).
    TTY& tty();

    /// Foreground process group (set via TIOCSPGRP).  Signal chars are
    /// delivered here; 0 = unset (falls back to the reader's group).
    int  foreground_pgid() const;
    void set_foreground_pgid(int pgid);

private:
    static void echo_via_kprintf(char c, void* ctx);

    TTY                tty_;
    cinux::proc::Task* reader_{nullptr};
    int                foreground_pgid_{0};
};

/// The system console TTY singleton.  Valid after ConsoleTty::init().
ConsoleTty& console_tty();

}  // namespace cinux::drivers
