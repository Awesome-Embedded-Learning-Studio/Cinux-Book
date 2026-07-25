/**
 * @file kernel/fs/devfs_init.cpp
 * @brief DevFS boot wiring: serial console sink + /dev mount (F6-M3)
 *
 * Kernel-only translation unit (linked into big_kernel_common, NOT into the
 * host unit tests).  Supplies the concrete CharSink that routes /dev/console
 * writes to the serial console, plus the devfs::init() boot hook that
 * constructs the DevFs, mounts it, and registers it at /dev.
 *
 * Kept separate from devfs.cpp so devfs.cpp stays host-linkable (zero kernel
 * I/O).  This is the §14 file-gate pattern: CMake decides whether devfs_init
 * compiles, so no #ifdef in source.
 *
 * Namespace: cinux::fs
 */

#include <stdint.h>

#include "kernel/drivers/block_registry.hpp"  // BlockRegistry (F6-M1 B1b: /dev/<name> nodes)
#include "kernel/drivers/input/input_event_device.hpp"  // /dev/event0 (F-GUI-USERSPACE b2)
#include "kernel/drivers/serial/serial.hpp"
#include "kernel/drivers/tty/console_tty.hpp"  // console_tty + console_tty_ioctl (B3b)
#include "kernel/drivers/tty/pty_device.hpp"   // /dev/ptmx clone + /dev/pts/N
#include "kernel/drivers/video/fb_dev.hpp"     // /dev/fb0 mmap (F-GUI-USERSPACE b1)
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/ipc/fifo.hpp"  // named FIFO dynamic lookup (F8-M2)
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/process.hpp"    // Task::controlling_tty (for /dev/tty)
#include "kernel/proc/scheduler.hpp"  // Scheduler::current()

namespace cinux::fs {
namespace {

/**
 * @brief CharSink that writes bytes to the serial console (COM1).
 *
 * The UART is initialised early in boot; this wrapper just drives putc()
 * byte-by-byte, mirroring how kprintf's serial sink (kprintf.cpp) operates --
 * same port, same polling, no re-init.
 */
class SerialConsoleSink : public CharSink {
public:
    cinux::lib::ErrorOr<int64_t> write(const void* buf, uint64_t count) override {
        if (buf == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        const auto* b = static_cast<const uint8_t*>(buf);
        for (uint64_t i = 0; i < count; ++i) {
            serial_.putc(static_cast<char>(b[i]));
        }
        return static_cast<int64_t>(count);
    }

private:
    cinux::drivers::Serial serial_{cinux::drivers::SERIAL_COM1};
};

/**
 * @brief ConsoleInput backend wiring /dev/console read+ioctl to the system
 * console TTY (B3b busybox-init).
 *
 * read() blocks on the console TTY's cooked line (a KERNEL buffer -- sys_read
 * hands InodeOps::read a kernel staging buffer, never the user pointer, so the
 * block is AC=0 safe with no sti/hlt in the syscall).  ioctl() delegates to the
 * shared console_tty_ioctl (TCGETS/TCSETS/TIOCGWINSZ/TIOCGPGRP/TIOCSPGRP/
 * TIOCSCTTY) -- the same handler sys_ioctl's fd 0/1/2 fallback uses.
 */
class ConsoleTtyInput : public ConsoleInput {
public:
    cinux::lib::ErrorOr<int64_t> read(void* buf, uint64_t count) override {
        if (buf == nullptr && count > 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        return static_cast<int64_t>(
            cinux::drivers::console_tty().read(static_cast<char*>(buf), count));
    }
    cinux::lib::ErrorOr<int64_t> ioctl(uint32_t request, uint64_t arg) override {
        return cinux::drivers::console_tty_ioctl(request, arg);
    }
};

// Boot-owned DevFs + console sink + console input.  Static locals live for the
// whole kernel run; the VFS mount table holds the DevFs pointer for the process
// lifetime.
SerialConsoleSink g_devfs_sink;
ConsoleTtyInput   g_devfs_input;
DevFs             g_devfs{&g_devfs_sink, &g_devfs_input};

// Resolve a dynamic /dev name the fixed DevFs node table cannot hold:
//   - a named FIFO (mkfifo-registered) -> its stable FIFO inode (F8-M2)
//   - "/dev/pts/<N>"                    -> the slave inode of PTY pair N
//   - "/dev/tty"                        -> the caller's controlling terminal
// Any other name -> NotFound.
cinux::lib::ErrorOr<Inode*> devfs_dynamic_lookup(const char* name) {
    if (name == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    // DevFs::lookup already stripped a leading '/', so expect "fifo", "tty",
    // or "pts/<N>".
    const char* p = name;
    if (p[0] == '/') {
        ++p;
    }
    // Named FIFO: consulted first, so FIFO names never collide with the PTY
    // fallbacks below.  NotFound falls through; any other error propagates.
    auto fifo = cinux::ipc::FifoRegistry::instance().lookup_inode(p);
    if (fifo.ok()) {
        return fifo.value();
    }
    if (fifo.error() != cinux::lib::Error::NotFound) {
        return fifo.error();
    }
    // /dev/tty -> the caller's controlling terminal.  In console-only boots
    // busybox init may spawn ash with stdio on /dev/console without issuing
    // TIOCSCTTY first, so no controlling tty is recorded; treat the built-in
    // console as the fallback controlling terminal until a PTY is acquired.
    if (p[0] == 't' && p[1] == 't' && p[2] == 'y' && p[3] == '\0') {
        cinux::proc::Task* task = cinux::proc::Scheduler::current();
        if (task == nullptr || task->controlling_tty < 0) {
            if (task != nullptr) {
                task->controlling_tty = cinux::drivers::kConsoleControllingTty;
                if (cinux::drivers::console_tty().foreground_pgid() == 0 && task->pgid != 0) {
                    cinux::drivers::console_tty().set_foreground_pgid(task->pgid);
                }
            }
            return g_devfs.lookup("console");
        }
        return cinux::drivers::pty_slave_inode(task->controlling_tty);
    }
    if (p[0] != 'p' || p[1] != 't' || p[2] != 's' || p[3] != '/') {
        return cinux::lib::Error::NotFound;
    }
    p += 4;  // past "pts/"
    if (*p < '0' || *p > '9') {
        return cinux::lib::Error::NotFound;
    }
    int index = 0;
    while (*p >= '0' && *p <= '9') {
        index = index * 10 + (*p - '0');
        ++p;
    }
    if (*p != '\0') {
        return cinux::lib::Error::NotFound;  // trailing junk
    }
    return cinux::drivers::pty_slave_inode(index);
}

}  // namespace

namespace devfs {

bool init() {
    if (!g_devfs.mount().ok()) {
        cinux::lib::kprintf("[DEVFS] mount failed\n");
        return false;
    }
    // PTY: /dev/ptmx is a cloning device (open() allocates a pair); /dev/pts/<N>
    // resolves dynamically to the matching slave inode.  Registered here so
    // devfs.cpp itself stays PTY-free (host-testable).
    g_devfs.add_node("ptmx", &cinux::drivers::ptmx_ops());
    // F-GUI-USERSPACE batch 1: /dev/fb0 -- mmap binds a user VMA to the VBE
    // framebuffer physical memory.
    g_devfs.add_node("fb0", &cinux::drivers::framebuffer_dev_ops());
    // F-GUI-USERSPACE batch 2: /dev/event0 -- userspace input device.  Mouse +
    // keyboard ISRs (via gui_init's listener) push Events; userspace reads them.
    g_devfs.add_node("event0", &cinux::input::input_event_device_ops());
    // F6-M1 B1b: register /dev/<name> for every block device in the registry.
    for (uint32_t i = 0; i < cinux::drivers::BlockRegistry::count(); ++i) {
        g_devfs.add_block_node(cinux::drivers::BlockRegistry::name_at(i),
                               cinux::drivers::BlockRegistry::device_at(i));
    }
    g_devfs.set_dynamic_lookup(&devfs_dynamic_lookup);
    if (!vfs_mount_add("/dev", &g_devfs)) {
        cinux::lib::kprintf("[DEVFS] vfs_mount_add /dev failed (table full?)\n");
        return false;
    }
    cinux::lib::kprintf("[DEVFS] mounted at /dev (%u nodes)\n", g_devfs.node_count());
    return true;
}

Inode* console_inode() {
    auto r = g_devfs.lookup("console");
    return r.ok() ? r.value() : nullptr;
}

DevFs* instance() {
    return g_devfs.is_mounted() ? &g_devfs : nullptr;
}

}  // namespace devfs
}  // namespace cinux::fs
