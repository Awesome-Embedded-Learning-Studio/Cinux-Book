/**
 * @file kernel/fs/devfs.hpp
 * @brief DevFS -- in-memory /dev filesystem with device-file inodes (F6-M3)
 *
 * DevFS is a virtual, memory-only filesystem (no on-disk backend) mounted at
 * /dev.  Each device node is an Inode whose ops pointer targets a
 * device-specific InodeOps subclass that encapsulates the device's read/write
 * behaviour.  This mirrors Linux's device-inode model: open("/dev/null")
 * resolves to a character-device inode whose fops discard writes and return
 * EOF on read, open("/dev/zero") yields zero bytes, open("/dev/console")
 * routes writes to the system console.
 *
 * The console device writes through an injected CharSink, so its dispatch
 * logic is pure and host-testable (a mock sink captures bytes) while the
 * kernel wires a concrete serial-backed sink at boot (devfs_init.cpp, F6-M3
 * batch 2).  Device inode lifetime is bound to the owning DevFs instance.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "fs/vfs_filesystem.hpp"

namespace cinux::fs {

/// Maximum length of a device node name, including the NUL terminator.
static constexpr uint32_t DEVFS_NAME_MAX = 32;

/// Maximum number of device nodes DevFS can register.
static constexpr uint32_t DEVFS_MAX_NODES = 16;

/// Linux st_mode file-type bits (subset of the S_IFMT family).  Device nodes
/// are character devices (S_IFCHR = 0x2000); the /dev directory is S_IFDIR.
static constexpr uint32_t kSIfChr = 0x2000;
static constexpr uint32_t kSIfDir = 0x4000;

/// Build a legacy Linux kdev_t-style device number (major:minor): minor in
/// bits 0..7, major in bits 8..15.  Hobby-OS simplification -- sufficient to
/// report a distinguishable st_rdev per device node.
constexpr uint64_t devfs_makedev(uint32_t major, uint32_t minor) {
    return (static_cast<uint64_t>(major) << 8) | static_cast<uint64_t>(minor & 0xFF);
}

/**
 * @brief Abstract byte sink for output-only character devices (e.g. /dev/console)
 *
 * The console device writes through this interface so its dispatch logic is
 * pure and host-testable (a mock captures bytes).  The kernel supplies a
 * concrete serial-backed sink at boot; this header stays kernel-decoupled.
 */
class CharSink {
public:
    virtual ~CharSink() = default;

    /// Write up to @p count bytes from @p buf; returns the byte count written.
    virtual cinux::lib::ErrorOr<int64_t> write(const void* buf, uint64_t count) = 0;
};

/**
 * @brief In-memory /dev filesystem (device-file inodes, no on-disk backend)
 *
 * Mounts at /dev.  mount() registers the standard nodes (/dev/null,
 * /dev/zero, /dev/console); lookup() resolves a name to the node's Inode.
 * The root directory inode supports readdir over the node table.  Device
 * nodes are Inodes whose ops point at device-specific InodeOps subclasses
 * (NullDevOps / ZeroDevOps / ConsoleDevOps) defined in devfs.cpp.
 *
 * Usage:
 *   DevFs devfs(&sink);
 *   devfs.mount();
 *   vfs_mount_add("/dev", &devfs);
 *   Inode* ino = devfs.lookup("null");  // -> character-device inode
 */
class DevFs : public FileSystem {
public:
    /// @param console_sink  Sink used by /dev/console writes; null = discard.
    explicit DevFs(CharSink* console_sink = nullptr);

    /// Releases the device ops instances allocated in mount().
    ~DevFs() override;

    cinux::lib::ErrorOr<void>   mount() override;
    cinux::lib::ErrorOr<Inode*> lookup(const char* path) override;

    /// Number of registered device nodes (excludes the root directory).
    uint32_t node_count() const { return node_count_; }

    /// Name of the i-th device node, or nullptr if @p i is out of range.
    /// Used by the directory readdir implementation.
    const char* node_name(uint32_t i) const;

private:
    /// A single device node: name plus its pre-allocated Inode.
    struct DevNode {
        char  name[DEVFS_NAME_MAX];
        Inode inode;
    };

    /// Register a character-device node with @p name backed by @p ops.
    void register_node(const char* name, InodeOps* ops);

    /// Fixed node table (each entry owns its Inode).
    DevNode  nodes_[DEVFS_MAX_NODES];
    uint32_t node_count_{0};

    // Device ops instances, owned.  Allocated once in mount() and live for the
    // DevFs lifetime -- mirrors Ramdisk's file_ops_/dir_ops_ ownership.
    InodeOps* null_ops_{nullptr};
    InodeOps* zero_ops_{nullptr};
    InodeOps* console_ops_{nullptr};
    InodeOps* dir_ops_{nullptr};

    /// Root directory inode (readdir walks nodes_ via fs_private == this).
    Inode root_inode_{};

    /// Sink wired into the console device; null => console writes discard.
    CharSink* console_sink_;
};

/**
 * @brief Boot hook: construct DevFs with a serial console sink and mount it at
 *        /dev.  Kernel-only implementation in devfs_init.cpp (linked into the
 *        kernel, not the host tests).
 *
 * @return true on success, false if mount() or vfs_mount_add(/dev) fails.
 */
namespace devfs {
bool init();
}  // namespace devfs

}  // namespace cinux::fs
