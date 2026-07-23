/**
 * @file kernel/drivers/tty/console_tty.hpp
 * @brief System console TTY singleton (F10-M3 batch 2)
 *
 * The single stdin the kernel shell reads from.  Keyboard bytes feed
 * input_char() (line discipline + echo); sys_read fd==0 drains read_cooked().
 * Owns the echo sink wiring (kprintf) + the VERASE tweak that matches the
 * keyboard driver's Backspace key, so tty.cpp itself stays Console/kprintf-free.
 */

#pragma once

#include "kernel/drivers/tty/tty.hpp"

namespace cinux::drivers {

/// The system console TTY (stdin).  Valid after console_tty_init().
TTY& console_tty();

/// Wire the console TTY: echo sink -> kprintf, VERASE = ^H (the keyboard
/// driver sends 0x08 for Backspace; the termios default is DEL 0x7F).  Call
/// once after kprintf/Console are up and before keyboard interrupts fire.
void console_tty_init();

/// Read a cooked line from stdin.  Blocks (sleeps the caller) until the line
/// discipline commits a line or EOF (^D on an empty line).  Returns the byte
/// count, or 0 on EOF.  Replaces the legacy keyboard busy-wait.
size_t console_tty_read(char* buf, size_t len);

/// Feed one keyboard byte to the line discipline (echo + editing + cooked
/// buffer).  Called from the keyboard IRQ handler; wakes a blocked reader when
/// a line or EOF is committed.
void console_tty_input(char c);

}  // namespace cinux::drivers
