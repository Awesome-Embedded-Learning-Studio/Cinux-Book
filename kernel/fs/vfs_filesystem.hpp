/**
 * @file kernel/fs/vfs_filesystem.hpp
 * @brief Abstract FileSystem base class for the VFS layer
 *
 * Every concrete filesystem backend (ramdisk, ext2, ...) must inherit
 * from FileSystem and implement the pure-virtual mount() and lookup()
 * methods.  The VFS mount table holds FileSystem pointers and dispatches
 * path-based lookups to the appropriate backend.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/fs/inode.hpp"

namespace cinux::fs {

/**
 * @brief Abstract base class for filesystem backends
 *
 * Provides the interface that the VFS mount layer uses to interact
 * with concrete filesystem implementations.  Each backend owns the
 * Inode objects it produces and is responsible for their lifetime.
 */
class FileSystem {
public:
    virtual ~FileSystem() = default;

    /**
     * @brief Mount (initialise) the filesystem backend
     *
     * Called once when the filesystem is added to the mount table.
     * The backend should locate its on-disk / in-memory data structures
     * and prepare for subsequent lookup() calls.
     *
     * @return ErrorOr<void> — Error::Ok on success, otherwise an Error code
     *         (e.g. Error::IOError) describing the failure
     */
    virtual cinux::lib::ErrorOr<void> mount() = 0;

    /**
     * @brief Look up a file by its path within this filesystem
     *
     * The path is relative to the mount point (the mount-layer strips
     * the mount prefix before calling this).
     *
     * @param path  Null-terminated path relative to the filesystem root
     * @return ErrorOr<Inode*> — the found Inode on success; Error::NotFound if
     *         no entry matches, Error::InvalidArgument for a null path
     */
    virtual cinux::lib::ErrorOr<Inode*> lookup(const char* path) = 0;

    /// F-USABILITY batch 1a: single-component lookup. Resolve one path
    /// component (name, namelen) within a parent directory inode. The vfs_lookup
    /// layer (kernel/fs/vfs_lookup) uses this to walk paths component-by-
    /// component and follow symlinks at the vfs level. Default returns
    /// NotImplemented; backends with a cheap single-step primitive (ext2
    /// lookup_in_dir, tmpfs find_child) override to enable full intermediate
    /// symlink following. Backends that only support whole-path lookup leave the
    /// default, and vfs_lookup falls back to lookup() (end-component follow
    /// only -- sufficient for virtual FSes that rarely have symlinks).
    virtual cinux::lib::ErrorOr<Inode*> lookup_child(const Inode* /*parent*/,
                                                     const char* /*name*/,
                                                     uint32_t /*namelen*/) {
        return cinux::lib::Error::NotImplemented;
    }

    /// Whether vfs_lookup may cache this filesystem's lookup_child results in
    /// the DentryCache.  On-disk FSes (ext2/tmpfs) return true (the default) --
    /// their entries are stable.  DevFs returns false: its lookups are dynamic
    /// (/dev/tty resolves to the CALLER's controlling terminal, /dev/pts/N to
    /// the N-th PTY), so caching the first result (e.g. shell 0's /dev/tty ->
    /// PTY 0) would make every later opener see shell 0's terminal.
    virtual bool dcache_enabled() const { return true; }
};

}  // namespace cinux::fs
