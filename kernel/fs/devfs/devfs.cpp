/**
 * @file kernel/fs/devfs.cpp
 * @brief DevFS implementation: device-file inodes + device InodeOps subclasses
 *
 * Defines the device behaviour as InodeOps subclasses (NullDevOps / ZeroDevOps
 * / ConsoleDevOps / DevDirOps) in an anonymous namespace, then the DevFs
 * FileSystem backend that owns the node table.  Pure logic only -- no kprintf,
 * no kernel-only I/O -- so this translation unit links cleanly into the host
 * unit tests (the console device reaches the outside world solely through the
 * injected CharSink).
 */

#include "devfs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/lib/string.hpp"

namespace cinux::fs {

namespace {

// ============================================================
// Shared helpers
// ============================================================

/// Fill a struct stat for a character-device inode.  Common to null/zero/
/// console: they differ only in st_rdev, which the caller supplies.
void fill_chardev_stat(const Inode* inode, struct stat* st, uint64_t rdev) {
    // Zero first so the Linux-ABI padding/_nsec/__unused fields stay 0
    // (no kernel-stack bytes leak to user space).
    memset(st, 0, sizeof(*st));
    st->st_dev     = 0;
    st->st_ino     = inode->ino;
    st->st_nlink   = 1;
    st->st_mode    = inode->mode;  // kSIfChr | perms, set at registration
    st->st_rdev    = rdev;
    st->st_size    = 0;
    st->st_blksize = 4096;
}

// ============================================================
// /dev/null (1:3) -- discard writes, read returns EOF
// ============================================================

class NullDevOps : public InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> read(const Inode*, uint64_t, void*, uint64_t) override {
        return 0;  // EOF: /dev/null is endlessly empty
    }
    cinux::lib::ErrorOr<int64_t> write(Inode*, uint64_t, const void*, uint64_t count) override {
        return static_cast<int64_t>(count);  // discard, claim all bytes written
    }
    cinux::lib::ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        fill_chardev_stat(inode, st, devfs_makedev(1, 3));
        return {};
    }
};

// ============================================================
// /dev/zero (1:5) -- reads yield zero bytes, writes discarded
// ============================================================

class ZeroDevOps : public InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> read(const Inode*, uint64_t, void* buf, uint64_t count) override {
        if (buf == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        memset(buf, 0, count);
        return static_cast<int64_t>(count);
    }
    cinux::lib::ErrorOr<int64_t> write(Inode*, uint64_t, const void*, uint64_t count) override {
        return static_cast<int64_t>(count);  // discard
    }
    cinux::lib::ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        fill_chardev_stat(inode, st, devfs_makedev(1, 5));
        return {};
    }
};

// ============================================================
// /dev/console (5:1) -- writes go to the injected CharSink
// ============================================================

class ConsoleDevOps : public InodeOps {
public:
    explicit ConsoleDevOps(CharSink* sink, ConsoleInput* input)
        : sink_(sink), input_(input) {}

    cinux::lib::ErrorOr<int64_t> read(const Inode*, uint64_t, void* buf, uint64_t count) override {
        // B3b: route /dev/console reads through the injected console backend
        // (busybox init's ash reads command lines this way).  No backend (host
        // tests / pre-boot) -> NotImplemented, preserving the prior
        // "output-only" behaviour for unit tests.
        if (input_ == nullptr) {
            return cinux::lib::Error::NotImplemented;
        }
        return input_->read(buf, count);
    }
    cinux::lib::ErrorOr<int64_t> ioctl(const Inode*, uint32_t request, uint64_t arg) override {
        // B3b: terminal ioctls (TCGETS / TIOCSCTTY / ...) reach the console TTY
        // via the injected backend.  No backend -> NotImplemented -> sys_ioctl
        // maps to -ENOTTY.
        if (input_ == nullptr) {
            return cinux::lib::Error::NotImplemented;
        }
        return input_->ioctl(request, arg);
    }
    cinux::lib::ErrorOr<int64_t> write(Inode*, uint64_t, const void* buf, uint64_t count) override {
        if (buf == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        if (sink_ == nullptr) {
            return static_cast<int64_t>(count);  // no sink wired: discard
        }
        return sink_->write(buf, count);
    }
    cinux::lib::ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        fill_chardev_stat(inode, st, devfs_makedev(5, 1));
        return {};
    }

private:
    CharSink*     sink_;   ///< Output sink; null => writes discard.
    ConsoleInput* input_;  ///< Read+ioctl backend (B3b); null => NotImplemented.
};

// ============================================================
// /dev directory -- readdir over the owning DevFs's node table
// ============================================================

class DevDirOps : public InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name,
                                         uint64_t name_max) override;
    cinux::lib::ErrorOr<void>    stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        memset(st, 0, sizeof(*st));
        st->st_ino     = inode->ino;
        st->st_nlink   = 2;
        st->st_mode    = kSIfDir | 0755;
        st->st_blksize = 4096;
        return {};
    }
};

cinux::lib::ErrorOr<int64_t> DevDirOps::readdir(const Inode* inode, uint64_t index, char* name,
                                                uint64_t name_max) {
    if (inode == nullptr || inode->fs_private == nullptr || name == nullptr || name_max == 0) {
        return cinux::lib::Error::InvalidArgument;
    }
    // fs_private points back at the owning DevFs (set in DevFs::mount).
    auto* fs = static_cast<const DevFs*>(inode->fs_private);

    if (index == 0) {
        if (name_max < 2) {
            return cinux::lib::Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        if (name_max < 3) {
            return cinux::lib::Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        return 1;
    }

    uint32_t i = static_cast<uint32_t>(index - 2);
    if (i >= fs->node_count()) {
        return 0;  // end of directory
    }
    const char* src = fs->node_name(i);
    uint64_t    n   = 0;
    while (src != nullptr && src[n] != '\0' && n + 1 < name_max) {
        name[n] = src[n];
        ++n;
    }
    name[n] = '\0';
    return 1;
}

}  // anonymous namespace

// ============================================================
// DevFs
// ============================================================

DevFs::DevFs(CharSink* console_sink, ConsoleInput* console_input)
    : console_sink_(console_sink), console_input_(console_input) {}

DevFs::~DevFs() {
    // ops_ are nullptr before mount() or already freed; deleting nullptr is safe.
    delete null_ops_;
    delete zero_ops_;
    delete console_ops_;
    delete dir_ops_;
}

cinux::lib::ErrorOr<void> DevFs::mount() {
    // Idempotent: a second mount() is a no-op.  Mirrors the re-entrancy safety
    // of Ramdisk::mount (which resets before re-parsing); without this guard a
    // repeat call would re-register the nodes and leak the first ops set.
    if (node_count_ > 0) {
        return {};
    }

    // One ops instance per device kind + the directory.  These outlive every
    // lookup result (Inode::ops points at them) and are freed in ~DevFs.
    null_ops_    = new NullDevOps();
    zero_ops_    = new ZeroDevOps();
    console_ops_ = new ConsoleDevOps(console_sink_, console_input_);
    dir_ops_     = new DevDirOps();

    register_node("null", null_ops_);
    register_node("zero", zero_ops_);
    register_node("console", console_ops_);

    // Root directory inode: readdir walks this DevFs's node table via fs_private.
    root_inode_.ino        = 0;
    root_inode_.type       = InodeType::Directory;
    root_inode_.ops        = dir_ops_;
    root_inode_.fs_private = this;
    root_inode_.mode       = kSIfDir | 0755;
    root_inode_.nlink      = 2;

    return {};
}

void DevFs::register_node(const char* name, InodeOps* ops) {
    if (name == nullptr || ops == nullptr || node_count_ >= DEVFS_MAX_NODES) {
        return;
    }
    auto&    node = nodes_[node_count_];
    uint32_t i    = 0;
    while (i + 1 < DEVFS_NAME_MAX && name[i] != '\0') {
        node.name[i] = name[i];
        ++i;
    }
    node.name[i]     = '\0';
    node.inode.ino   = static_cast<uint64_t>(node_count_) + 1;  // 1-based; 0 = root
    node.inode.type  = InodeType::Regular;
    node.inode.ops   = ops;
    node.inode.mode  = kSIfChr | 0666;
    node.inode.nlink = 1;
    ++node_count_;
}

const char* DevFs::node_name(uint32_t i) const {
    if (i >= node_count_) {
        return nullptr;
    }
    return nodes_[i].name;
}

void DevFs::add_node(const char* name, InodeOps* ops) {
    register_node(name, ops);
}

void DevFs::set_dynamic_lookup(DynamicLookup resolver) {
    dynamic_lookup_ = resolver;
}

cinux::lib::ErrorOr<Inode*> DevFs::lookup(const char* path) {
    if (path == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    // Root directory: empty path or "/".
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return &root_inode_;
    }
    // Strip a single leading '/': vfs_resolve may pass "/null" (mount prefix
    // without trailing slash) or "null" (with trailing slash).
    if (path[0] == '/') {
        ++path;
    }

    for (uint32_t i = 0; i < node_count_; ++i) {
        const char* a = nodes_[i].name;
        uint32_t    j = 0;
        while (a[j] != '\0' && path[j] != '\0') {
            if (a[j] != path[j]) {
                break;
            }
            ++j;
        }
        if (a[j] == '\0' && path[j] == '\0') {
            return &nodes_[i].inode;
        }
    }

    // No static node matched.  Defer to the dynamic resolver if boot wired one
    // (the PTY registry answers /dev/pts/<N> here, on demand).
    if (dynamic_lookup_ != nullptr) {
        return dynamic_lookup_(path);
    }
    return cinux::lib::Error::NotFound;
}

}  // namespace cinux::fs
