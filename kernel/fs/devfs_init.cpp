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
#include "kernel/fs/devfs.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"

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

}  // namespace

namespace devfs {

bool init() {
    if (!g_devfs.mount().ok()) {
        cinux::lib::kprintf("[DEVFS] mount failed\n");
        return false;
    }
    if (!vfs_mount_add("/dev", &g_devfs)) {
        cinux::lib::kprintf("[DEVFS] vfs_mount_add /dev failed (table full?)\n");
        return false;
    }
    cinux::lib::kprintf("[DEVFS] mounted at /dev (%u nodes)\n", g_devfs.node_count());
    return true;
}

}  // namespace devfs
}  // namespace cinux::fs
