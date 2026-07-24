/**
 * @file kernel/fs/dentry.hpp
 * @brief DentryCache -- VFS directory-entry cache (F6-M1 B3)
 *
 * Caches (parent Inode*, name) -> child Inode* so vfs_lookup skips the on-disk
 * lookup_child on a hit. Entries pin their child via inode_ref (the child cannot
 * be freed while cached); invalidate() (sys_unlink / rmdir / rename) drops the
 * entry and unpins. There is no LRU shrink yet -- entries accumulate over the
 * boot, which is fine for a hobby OS; a bounded cache is a follow-up.
 *
 * Key invariant: the same logical directory yields the SAME Inode* across
 * lookups (ext2 inode_cache, tmpfs TmpNode-embedded inodes, procfs/devfs fixed
 * pools all guarantee this), so (parent Inode*, name) is a stable key.
 *
 * Namespace: cinux::fs
 */
#pragma once

#include <stdint.h>

namespace cinux::fs {

struct Inode;

class DentryCache {
public:
    /// Hit -> inode_ref'd child (caller owns the ref); miss -> nullptr.
    static Inode* lookup(const Inode* parent, const char* name, uint32_t namelen);

    /// Cache (parent, name) -> child. Pins child via inode_ref. Replaces any
    /// existing entry for the same (parent, name).
    static void add(const Inode* parent, const char* name, uint32_t namelen, Inode* child);

    /// Remove the (parent, name) entry (unpins its child). No-op if absent.
    /// Called from sys_unlink / sys_rmdir / sys_rename after a successful op.
    static void invalidate(const Inode* parent, const char* name, uint32_t namelen);

    /// Cached entry count (diagnostic / tests).
    static uint32_t count();
};

}  // namespace cinux::fs
