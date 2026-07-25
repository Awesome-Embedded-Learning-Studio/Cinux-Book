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

#include "kernel/drivers/tty/console_tty.hpp"  // console_tty_ioctl (shared handler)
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"       // FDTable / File (fd -> inode)
#include "kernel/fs/vfs_mount.hpp"  // current_fd_table()

namespace cinux::syscall {

namespace {
// Legacy boot/test fds 0/1/2 can still back onto the system console TTY when no
// real File is installed in the current FDTable.  If a File exists, sys_ioctl()
// dispatches to that inode first.
bool is_console_tty_fd(uint64_t fd) {
    return fd <= 2;
}

// Terminal ioctls on the console TTY (fds 0/1/2 legacy fallback).  B3b: the
// per-request logic now lives in console_tty_ioctl (shared with the /dev/console
// inode), so this fallback is just the ErrorOr -> -errno adapter.  Returns the
// raw value the syscall hands back to user space: 0 on success, -errno on
// failure.
int64_t console_ioctl(uint32_t request, void* uptr) {
    // B3b: the per-request logic lives in console_tty_ioctl (shared with the
    // /dev/console inode).  Map its ErrorOr back into the -errno convention:
    // NotImplemented -> -ENOTTY (unknown request); everything else goes through
    // to_errno (Fault -> EFAULT, InvalidArgument -> EINVAL).
    auto r = cinux::drivers::console_tty_ioctl(request, reinterpret_cast<uint64_t>(uptr));
    if (r.ok()) {
        return *r;
    }
    if (r.error() == cinux::lib::Error::NotImplemented) {
        return -cinux::kEnotty;
    }
    return -cinux::to_errno(r.error());
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
