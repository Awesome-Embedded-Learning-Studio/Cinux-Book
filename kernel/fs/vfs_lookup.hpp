/**
 * @file kernel/fs/vfs_lookup.hpp
 * @brief VFS path resolution with symlink following (F-USABILITY batch 1b)
 *
 * Single entry point for resolving a path to an Inode with Linux-style symlink
 * semantics: intermediate components are always followed, the trailing
 * component is followed iff LookupFlag::Follow is set. Replaces the per-syscall
 * vfs_resolve + fs->lookup + split_pathname pattern so every path syscall shares
 * one resolver and one follow implementation (the symlink-follow debt memory
 * F-ECO b2 tracked).
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/fs/inode.hpp"

namespace cinux::fs {

/// F-USABILITY batch 1b: flags controlling vfs_lookup path resolution.
enum class LookupFlag : uint32_t {
    None      = 0,
    /// Follow a trailing symlink (open/stat/execve default).
    Follow    = 1u << 0,
    /// Target must be a directory (chdir).
    Directory = 1u << 1,
    /// Stop one component early: return the parent directory + the leaf name
    /// (the leaf is NOT looked up). Used by create-style syscalls (mkdir /
    /// unlink / symlink / link / rename / creat).
    Parent    = 1u << 2,
    Create    = 1u << 3,  ///< Advisory: O_CREAT semantics handled by the caller.
    Excl      = 1u << 4,  ///< Advisory: O_EXCL semantics handled by the caller.
    /// Do NOT follow a trailing symlink (readlink / lstat).
    NoFollow  = 1u << 5,
};

struct LookupResult {
    Inode*      target{nullptr};  ///< Resolved target (non-PARENT mode).
    Inode*      parent{nullptr};  ///< Parent directory (PARENT mode).
    const char* leaf{nullptr};    ///< Leaf component (PARENT); pointer into the path buffer.
    uint32_t    leaf_len{0};
};

/// Resolve @p path (relative to @p cwd) to an Inode, following symlinks.
/// Intermediate symlink components are always followed; the trailing component
/// is followed iff Follow is set (and NoFollow is clear). Caps total symlink
/// follows at 40 (Linux MAXSYMLINKS) -- beyond that returns Error::Loop.
///
/// @returns LookupResult, or Error::NotFound / NotADirectory / Loop / IOError /
///          InvalidArgument.
cinux::lib::ErrorOr<LookupResult> vfs_lookup(const char* path, uint32_t flags,
                                             const char* cwd);

}  // namespace cinux::fs
