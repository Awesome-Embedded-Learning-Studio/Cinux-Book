/**
 * @file kernel/syscall/sys_ioctl.cpp
 * @brief sys_ioctl handler implementation (F10-M3 batch 4/5)
 *
 * Console TTY ioctls on fds 0/1/2 (the three standard streams that back onto
 * the system console line discipline):
 *   - TCGETS/TCSETS: read/write termios.
 *   - TIOCGWINSZ: window size.  musl/glibc probe it on the first stdout write
 *     to pick line vs full buffering, so answering it makes stdio flush
 *     promptly instead of buffering whole programs.
 *   - TIOCGPGRP/TIOCSPGRP (batch 5): read/write the foreground process group
 *     that interrupt/quit/suspend signal chars (delivered by ConsoleTty::input)
 *     target.
 *
 * SMAP-safe: every payload crosses the user/kernel boundary through
 * copy_to/from_user (the F-EXTABLE extable-annotated accessors), so a bad user
 * pointer faults into -EFAULT instead of panicking.
 *
 * Deferred: TIOCSCTTY + the rest of job control need DevFS (F6); every other
 * request stays -ENOTTY.
 */

#include "kernel/syscall/sys_ioctl.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (SMAP/extable)
#include "kernel/drivers/tty/console_tty.hpp"  // console_tty() singleton
#include "kernel/drivers/tty/tty.hpp"          // Termios/Winsize/ioctl UAPI consts
#include "kernel/errno.hpp"

namespace cinux::syscall {

using cinux::drivers::Termios;
using cinux::drivers::Winsize;
using cinux::drivers::console_tty;
using cinux::drivers::kTcgets;
using cinux::drivers::kTcsets;
using cinux::drivers::kTiocgwinsz;
using cinux::drivers::kTiocgpgrp;
using cinux::drivers::kTiocspgrp;
using cinux::user::copy_from_user;
using cinux::user::copy_to_user;

namespace {
// fds 0/1/2 (stdin/stdout/stderr) all back onto the system console TTY. Until
// DevFS (F6) gives file descriptors real device identity, only these three
// answer terminal ioctls; anything else is -ENOTTY (not a tty).
bool is_console_tty_fd(uint64_t fd) {
    return fd <= 2;
}

// Console geometry. The live Console (a main.cpp local that knows the
// framebuffer-derived cols/rows) is not globally reachable from the syscall
// layer; 80x25 is the classic Linux text-mode default and is all libc needs to
// choose a buffering mode + wrap width. Surfacing the real geometry is a
// follow-up.
constexpr Winsize kConsoleWinsize{25, 80, 0, 0};
}  // namespace

int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg, uint64_t, uint64_t, uint64_t) {
    if (!is_console_tty_fd(fd)) {
        return -cinux::kEnotty;
    }

    void* uptr = reinterpret_cast<void*>(arg);

    switch (request) {
    case kTcgets: {
        // Hand the caller the console TTY's current termios.
        const Termios& tm = console_tty().tty().termios();
        if (!copy_to_user(uptr, &tm, sizeof(Termios))) {
            return -cinux::kEfault;
        }
        return 0;
    }
    case kTcsets: {
        // Adopt a caller-supplied termios; set_termios discards the line
        // being edited (matches Linux: a termios change invalidates it).
        Termios tm;
        if (!copy_from_user(&tm, uptr, sizeof(Termios))) {
            return -cinux::kEfault;
        }
        console_tty().tty().set_termios(tm);
        return 0;
    }
    case kTiocgwinsz: {
        if (!copy_to_user(uptr, &kConsoleWinsize, sizeof(Winsize))) {
            return -cinux::kEfault;
        }
        return 0;
    }
    case kTiocgpgrp: {
        // Read the console TTY's foreground process group.
        int pgid = console_tty().foreground_pgid();
        if (!copy_to_user(uptr, &pgid, sizeof(int))) {
            return -cinux::kEfault;
        }
        return 0;
    }
    case kTiocspgrp: {
        // Install a caller-supplied foreground process group.
        int pgid;
        if (!copy_from_user(&pgid, uptr, sizeof(int))) {
            return -cinux::kEfault;
        }
        if (pgid < 0) {
            return -cinux::kEinval;
        }
        console_tty().set_foreground_pgid(pgid);
        return 0;
    }
    default:
        // Unknown request -- not a terminal ioctl this driver handles.
        return -cinux::kEnotty;
    }
}

}  // namespace cinux::syscall
