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

#include <cinux/expected.hpp>  // ErrorOr (console_tty_ioctl return)
#include <cstddef>
#include <cstdint>

#include "kernel/drivers/tty/tty.hpp"

namespace cinux::proc {
struct Task;  // the blocked stdin reader; full def in process.hpp
}

namespace cinux::drivers {

/// Task::controlling_tty value for the built-in system console.  PTY slaves use
/// non-negative indices; -1 remains "no controlling terminal".
constexpr int kConsoleControllingTty = -2;

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

    /// poll/select readiness for stdin (F8-M5).  Returns POLLIN when a cooked
    /// line / EOF is ready; otherwise parks @p waiter in the single reader slot
    /// (the same one read() uses) so the keyboard feeder wakes it on the next
    /// committed line.  *@p registered is set iff a waiter was parked.
    uint32_t poll_events(cinux::proc::Task* waiter, bool* registered);

    /// Remove a poll waiter parked by poll_events() (no-op if @p waiter is not
    /// the parked reader).  poll calls this after it wakes.
    void poll_detach(cinux::proc::Task* waiter);

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

/// Shared terminal-ioctl handler for the system console TTY (B3b busybox-init).
/// One implementation of TCGETS / TCSETS / TIOCGWINSZ / TIOCGPGRP / TIOCSPGRP /
/// TIOCSCTTY consumed by BOTH callers that reach the console TTY:
///   - sys_ioctl's legacy fd 0/1/2 fallback (no File installed), and
///   - the /dev/console inode (devfs ConsoleDevOps::ioctl, after busybox init
///     open("/dev/console") + dup 0/1/2).
/// @p arg is the opaque USER payload; every crossing goes through
/// copy_to/from_user (SMAP/extable-safe).  Returns ErrorOr<int64_t>: success
/// -> 0; NotImplemented -> the caller maps to -ENOTTY (unknown request);
/// Fault -> -EFAULT (bad user pointer, extable fixup); InvalidArgument ->
/// -EINVAL (bad arg, e.g. negative pgid).  TIOCSCTTY uses minimal
/// accept-without-steal semantics (a B3b follow-up item): busybox init calls it
/// once after setsid() and the spawned ash needs it to read through
/// /dev/console.
cinux::lib::ErrorOr<int64_t> console_tty_ioctl(uint32_t request, uint64_t arg);

}  // namespace cinux::drivers
