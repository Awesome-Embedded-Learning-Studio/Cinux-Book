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

namespace cinux::drivers {
class IBlockDevice;  // forward -- add_block_node wires a block-device node
}

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
 * @brief Optional console read/ioctl backend for /dev/console (B3b busybox-init)
 *
 * CharSink above only covers writes.  busybox init opens /dev/console, dups it
 * onto fds 0/1/2, setsid + TIOCSCTTY, and the ash it forks calls isatty
 * (TCGETS) and reads command lines -- all of which route through the console
 * inode's ops.  This interface injects the kernel-side console TTY (cooked
 * stdin + terminal ioctls) so devfs.cpp stays host-testable (no console_tty /
 * user-access dependency); the kernel-only devfs_init.cpp supplies the concrete
 * implementation.  When null (host unit tests, or before boot wiring),
 * /dev/console read/ioctl return NotImplemented.
 */
class ConsoleInput {
public:
    virtual ~ConsoleInput() = default;

    /// Read a cooked line into the KERNEL buffer @p buf (InodeOps::read hands a
    /// kernel staging buffer, never a user pointer).  Returns the byte count.
    virtual cinux::lib::ErrorOr<int64_t> read(void* buf, uint64_t count) = 0;

    /// Terminal ioctl (TCGETS / TCSETS / TIOCGWINSZ / TIOCGPGRP / TIOCSPGRP /
    /// TIOCSCTTY).  @p arg is the opaque USER payload; the implementation
    /// crosses the user/kernel boundary itself (copy_to/from_user).
    virtual cinux::lib::ErrorOr<int64_t> ioctl(uint32_t request, uint64_t arg) = 0;
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
    /// @param console_sink   Sink used by /dev/console writes; null = discard.
    /// @param console_input  Optional /dev/console read+ioctl backend (B3b);
    ///                       null => read/ioctl return NotImplemented (host).
    explicit DevFs(CharSink* console_sink = nullptr, ConsoleInput* console_input = nullptr);

    /// Releases the device ops instances allocated in mount().
    ~DevFs() override;

    cinux::lib::ErrorOr<void>   mount() override;
    cinux::lib::ErrorOr<Inode*> lookup(const char* path) override;
    // Single-component lookup for the vfs_lookup resolver (F-USABILITY batch 4):
    // without this the FileSystem base returns ENOSYS and open("/dev/null") etc.
    // fails.  DevFs is flat (root directory + device nodes only).
    cinux::lib::ErrorOr<Inode*> lookup_child(const Inode* parent, const char* name,
                                             uint32_t namelen) override;

    /// DevFs lookups are dynamic (/dev/tty -> caller's controlling terminal,
    /// /dev/pts/N -> N-th PTY), so the DentryCache must not pin the first
    /// resolution -- otherwise every opener after the first sees the first
    /// opener's terminal (multi-shell SIGTTIN bug).
    bool dcache_enabled() const override { return false; }

    /// Number of registered device nodes (excludes the root directory).
    uint32_t node_count() const { return node_count_; }

    /// Whether mount() has registered nodes (F6-M1: devfs::instance() exposes
    /// the singleton only once mounted).
    bool is_mounted() const { return node_count_ > 0; }

    /// Name of the i-th device node, or nullptr if @p i is out of range.
    /// Used by the directory readdir implementation.
    const char* node_name(uint32_t i) const;

    /// Register an extra static device node after mount().  Boot wiring for a
    /// node whose ops live outside devfs.cpp (e.g. /dev/ptmx, whose open() is a
    /// PTY clone).  No-op if the table is full.
    void add_node(const char* name, InodeOps* ops);

    /// F6-M1 B1b: register a block-device node backed by @p dev.  The node's
    /// InodeOps::block_device() override exposes @p dev so sys_mount can resolve
    /// a source path (e.g. /dev/sda) to the IBlockDevice to mount.  The per-node
    /// BlockDevOps instance is owned by this DevFs and freed in ~DevFs.  No-op if
    /// @p dev is null or the block-node table is full.
    void add_block_node(const char* name, cinux::drivers::IBlockDevice* dev);

    /// Dynamic per-call resolver for names not in the static table -- the
    /// /dev/pts/<N> slave inodes are allocated on demand, so they cannot live in
    /// the fixed node array.  devfs_init wires this to the PTY registry; devfs.cpp
    /// itself stays PTY-free (and thus host-testable).
    using DynamicLookup = cinux::lib::ErrorOr<Inode*> (*)(const char* name);
    void set_dynamic_lookup(DynamicLookup resolver);

private:
    /// A single device node: name plus its pre-allocated Inode.
    struct DevNode {
        char  name[DEVFS_NAME_MAX];
        Inode inode;
    };

    /// Register a character-device node with @p name backed by @p ops.
    void register_node(const char* name, InodeOps* ops);

    /// Optional resolver for dynamic names (set via set_dynamic_lookup).  lookup
    /// falls back to it when the static node table has no match.
    DynamicLookup dynamic_lookup_{nullptr};

    /// Fixed node table (each entry owns its Inode).
    DevNode  nodes_[DEVFS_MAX_NODES];
    uint32_t node_count_{0};

    /// Block-device nodes added via add_block_node (F6-M1 B1b).  Each entry owns
    /// a heap-allocated BlockDevOps freed in ~DevFs.
    static constexpr uint32_t MAX_BLOCK_NODES = 8;
    InodeOps* block_dev_ops_[MAX_BLOCK_NODES]{};
    uint32_t  block_dev_count_{0};

    // Device ops instances, owned.  Allocated once in mount() and live for the
    // DevFs lifetime -- mirrors Ramdisk's file_ops_/dir_ops_ ownership.
    InodeOps* null_ops_{nullptr};
    InodeOps* zero_ops_{nullptr};
    InodeOps* console_ops_{nullptr};
    InodeOps* dir_ops_{nullptr};

    /// Root directory inode (readdir walks nodes_ via fs_private == this).
    Inode root_inode_{};

    /// Virtual /dev/pts directory inode.  DevFs is otherwise flat, but the vfs
    /// resolver walks paths component-by-component, so "pts" must resolve to a
    /// directory it can descend into before looking up "<N>".  Without it
    /// open("/dev/pts/N") dies at the "pts" component (dynamic_lookup_ expects
    /// the whole "pts/N" spelling) and the slave fd is never returned -- the
    /// shell then inherits /dev/console and all IO bypasses the PTY.
    Inode pts_dir_inode_{};

    /// Sink wired into the console device; null => console writes discard.
    CharSink*     console_sink_;
    /// Read+ioctl backend for /dev/console (B3b); null => read/ioctl return
    /// NotImplemented (host unit tests).
    ConsoleInput* console_input_;
};

/**
 * @brief Boot hook: construct DevFs with a serial console sink and mount it at
 *        /dev.  Kernel-only implementation in devfs_init.cpp (linked into the
 *        kernel, not the host tests).
 *
 * @return true on success, false if mount() or vfs_mount_add(/dev) fails.
 */
namespace devfs {
bool    init();
Inode*  console_inode();
DevFs*  instance();  ///< Boot-owned DevFs singleton (F6-M1: sys_mount -t devfs).
                     ///< nullptr before devfs::init().
}  // namespace devfs

}  // namespace cinux::fs
