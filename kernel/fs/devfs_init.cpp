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

#include "kernel/drivers/serial/serial.hpp"
#include "kernel/drivers/tty/pty_device.hpp"  // /dev/ptmx clone + /dev/pts/N
#include "kernel/fs/devfs.hpp"
#include "kernel/fs/vfs_mount.hpp"
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

// Boot-owned DevFs + console sink.  Static locals live for the whole kernel
// run; the VFS mount table holds the DevFs pointer for the process lifetime.
SerialConsoleSink g_devfs_sink;
DevFs             g_devfs{&g_devfs_sink};

// Resolve a dynamic /dev name the fixed DevFs node table cannot hold.  Today
// this is just "/dev/pts/<N>" -> the slave inode of PTY pair N (allocated on
// demand by /dev/ptmx open).  Any other name -> NotFound.
cinux::lib::ErrorOr<Inode*> pty_dynamic_lookup(const char* name) {
    if (name == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    // DevFs::lookup already stripped a leading '/', so expect "tty" or "pts/<N>".
    const char* p = name;
    if (p[0] == '/') {
        ++p;
    }
    // /dev/tty -> the caller's controlling terminal (the slave side of its PTY).
    if (p[0] == 't' && p[1] == 't' && p[2] == 'y' && p[3] == '\0') {
        cinux::proc::Task* task = cinux::proc::Scheduler::current();
        if (task == nullptr || task->controlling_tty < 0) {
            return cinux::lib::Error::NotFound;  // no controlling terminal
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
    g_devfs.set_dynamic_lookup(&pty_dynamic_lookup);
    if (!vfs_mount_add("/dev", &g_devfs)) {
        cinux::lib::kprintf("[DEVFS] vfs_mount_add /dev failed (table full?)\n");
        return false;
    }
    cinux::lib::kprintf("[DEVFS] mounted at /dev (%u nodes)\n", g_devfs.node_count());
    return true;
}

}  // namespace devfs
}  // namespace cinux::fs
