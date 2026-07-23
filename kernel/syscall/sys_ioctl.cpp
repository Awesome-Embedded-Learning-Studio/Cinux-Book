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
 * Dispatch: an fd table entry always wins, including fd 0/1/2.  GUI shells bind
 * their standard streams to PTY slave files, and terminal ioctls must reach that
 * slave so line editors can change ICANON/ECHO.  Only absent legacy stdio fds
 * fall back to the global console TTY path.
 */

#include "kernel/syscall/sys_ioctl.hpp"

#include <stdint.h>

#include <cinux/expected.hpp>  // cinux::lib::Error (NotImplemented -> ENOTTY mapping)

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (SMAP/extable)
#include "kernel/drivers/tty/console_tty.hpp"  // console_tty() singleton
#include "kernel/drivers/tty/tty.hpp"          // Termios/Winsize/ioctl UAPI consts
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"       // FDTable / File (fd -> inode)
#include "kernel/fs/vfs_mount.hpp"  // current_fd_table()

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
// Legacy boot/test fds 0/1/2 can still back onto the system console TTY when no
// real File is installed in the current FDTable.  If a File exists, sys_ioctl()
// dispatches to that inode first.
bool is_console_tty_fd(uint64_t fd) {
    return fd <= 2;
}

// Console geometry. The live Console (a main.cpp local that knows the
// framebuffer-derived cols/rows) is not globally reachable from the syscall
// layer; 80x25 is the classic Linux text-mode default and is all libc needs to
// choose a buffering mode + wrap width. Surfacing the real geometry is a
// follow-up.
constexpr Winsize kConsoleWinsize{25, 80, 0, 0};

// Terminal ioctls on the console TTY (fds 0/1/2). Returns the raw value the
// syscall hands back to user space: 0 on success, -errno on failure. Extracted
// unchanged from the old monolithic sys_ioctl so the fd>2 dispatch path could
// be added without touching console behaviour.
int64_t console_ioctl(uint32_t request, void* uptr) {
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
}  // namespace

int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg, uint64_t, uint64_t, uint64_t) {
    void* uptr = reinterpret_cast<void*>(arg);

    // Dispatch through an installed File first.  This deliberately includes
    // fd 0/1/2: a GUI shell's stdio fds are PTY slave inodes, not the global
    // console.  If TCSETS were sent to the console fallback, the PTY would keep
    // ECHO enabled and busybox line editing would double-echo command lines.
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        auto r = file->inode->ops->ioctl(file->inode, static_cast<uint32_t>(request), arg);
        if (r.ok()) {
            return *r;
        }
        if (r.error() == cinux::lib::Error::NotImplemented) {
            return -cinux::kEnotty;  // inode type does not implement ioctls
        }
        return -cinux::to_errno(r.error());
    }

    // Legacy fds 0/1/2 with no fd-table entry still behave as the boot console.
    if (is_console_tty_fd(fd)) {
        return console_ioctl(static_cast<uint32_t>(request), uptr);
    }

    return -cinux::kEnotty;
}

}  // namespace cinux::syscall
