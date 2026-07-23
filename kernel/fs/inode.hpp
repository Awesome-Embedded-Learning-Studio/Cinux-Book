/**
 * @file kernel/fs/inode.hpp
 * @brief VFS inode definitions -- the core abstraction for filesystem objects
 *
 * Defines InodeType (regular file, directory, etc.), the InodeOps virtual
 * table for per-inode operations (read, write, readdir), and the Inode
 * struct itself which ties everything together.
 *
 * Each concrete filesystem (ramdisk, ext2, ...) produces Inode instances
 * whose ops pointers point at filesystem-specific implementations.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "fs/stat.hpp"

namespace cinux::fs {

// ============================================================
// Inode Type Enumeration
// ============================================================

/// Type of filesystem object represented by an inode
enum class InodeType : uint8_t {
    Unknown   = 0,
    Regular   = 1,
    Directory = 2,
};

// ============================================================
// Inode Operations (virtual function table)
// ============================================================

struct Inode;

/**
 * @brief Abstract base class for inode-level operations
 *
 * Each concrete filesystem provides InodeOps subclasses that implement
 * read, write, readdir, create, mkdir, and unlink.  Unsupported
 * operations fall back to the default implementations (returning -1
 * or nullptr).
 */
class InodeOps {
public:
    virtual ~InodeOps() = default;

    virtual cinux::lib::ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf,
                                              uint64_t count);
    virtual cinux::lib::ErrorOr<int64_t> write(Inode* inode, uint64_t offset, const void* buf,
                                               uint64_t count);
    virtual cinux::lib::ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name,
                                                 uint64_t name_max);
    virtual cinux::lib::ErrorOr<Inode*>  create(Inode* dir, const char* name, uint32_t namelen);
    virtual cinux::lib::ErrorOr<Inode*>  mkdir(Inode* dir, const char* name, uint32_t namelen);
    virtual cinux::lib::ErrorOr<void>    unlink(Inode* dir, const char* name, uint32_t namelen);
    virtual cinux::lib::ErrorOr<void>    stat(const Inode* inode, struct stat* st);

    /// Device-specific ioctl (terminal ioctls on a PTY/console inode, ...).
    /// @p request is the Linux ioctl request word (TCGETS, TIOCSCTTY, ...);
    /// @p arg the opaque user payload.  The default returns NotImplemented;
    /// sys_ioctl maps that to -ENOTTY, so an inode type that does not override
    /// this simply answers "not a tty ioctl".  Overrides cross the user/kernel
    /// boundary themselves (copy_to/from_user).
    virtual cinux::lib::ErrorOr<int64_t> ioctl(const Inode* inode, uint32_t request, uint64_t arg);

    /// Called by sys_open after lookup resolves this inode.  The default returns
    /// the same inode (bind the fd to what lookup found).  A cloning device --
    /// Linux /dev/ptmx is the classic case -- overrides this to allocate a fresh
    /// per-open resource (a PTY pair) and return a distinct inode (the master
    /// end) for the new fd.  @p flags is the raw Linux open() flag word so a
    /// cloning device can honour direction / O_NONBLOCK (a FIFO's open() uses it
    /// to pick the read or write end of the underlying pipe).  Returning an
    /// error fails the open.
    virtual cinux::lib::ErrorOr<Inode*> open(Inode* inode, uint64_t flags);

    // ============================================================
    // F-ECO batch 2: attribute + dirent operations (default-backed).
    //
    // Each of these has a default that returns NotImplemented, exactly like
    // ioctl()/open() above, so the ~20 existing InodeOps subclasses
    // (pipe/pty/devfs/procfs/ramdisk/socket/...) need no change; ext2 is the
    // only backend that overrides them for now. They are added in one shot so
    // the parallel implementation of the syscalls never edits this shared
    // header again.
    // ============================================================

    /// Change the permission bits of an inode (sys_chmod). Only the low 12
    /// permission bits of @p mode take effect; the file-type bits are kept.
    virtual cinux::lib::ErrorOr<void> chmod(Inode* inode, uint32_t mode);

    /// Change the owner of an inode (sys_chown). A @p uid or @p gid of
    /// 0xFFFFFFFF ((uint32_t)-1) leaves that field untouched, per Linux chown(2).
    virtual cinux::lib::ErrorOr<void> chown(Inode* inode, uint32_t uid, uint32_t gid);

    /// Set access / modification times (sys_utimensat). ext2 revision-0 inodes
    /// store whole seconds; the nsec fields are accepted but truncated.
    virtual cinux::lib::ErrorOr<void> utimensat(Inode* inode, uint64_t atime_sec,
                                                uint32_t atime_nsec, uint64_t mtime_sec,
                                                uint32_t mtime_nsec);

    /// Read a symbolic link's target into @p buf (sys_readlink). Returns the
    /// number of bytes written to @p buf (NOT counting a trailing NUL).
    virtual cinux::lib::ErrorOr<int64_t> readlink(const Inode* inode, char* buf,
                                                  uint64_t buf_size);

    /// Create a symbolic link named @p name in directory @p dir pointing at the
    /// NUL-terminated @p target string (sys_symlink).
    virtual cinux::lib::ErrorOr<void> symlink(Inode* dir, const char* name, uint32_t namelen,
                                              const char* target);

    /// Create a hard link named @p name in directory @p dir referring to @p
    /// target (sys_link): adds a directory entry and bumps the target's nlink.
    virtual cinux::lib::ErrorOr<void> link(Inode* dir, const char* name, uint32_t namelen,
                                           const Inode* target);

    /// Rename entry @p src_name in @p src_dir to @p dst_name in @p dst_dir
    /// (sys_rename). @p dst_dir may equal @p src_dir. Hobby-OS move: removes the
    /// old directory entry and adds a fresh one; no atomic cross-directory swap.
    virtual cinux::lib::ErrorOr<void> rename(Inode* src_dir, const char* src_name,
                                             uint32_t src_namelen, Inode* dst_dir,
                                             const char* dst_name, uint32_t dst_namelen);

    /// Whether reads against this inode should be served through the file-backed
    /// PageCache.  Disk-backed filesystems (ext2) override to true so that
    /// sys_read and demand paging share one cache; transient inode-ops shims
    /// such as pipes inherit the default false (their content is not on disk and
    /// must never be cached).  Default false keeps every legacy/mock backend
    /// unchanged.
    virtual bool is_page_cacheable() const;
};

// ============================================================
// Inode Structure
// ============================================================

/**
 * @brief Represents a single filesystem object (file, directory, etc.)
 *
 * Inodes are produced by concrete FileSystem backends during lookup().
 * They are owned by the producing filesystem and must not be freed by
 * the caller -- the filesystem manages their lifetime.
 */
struct Inode {
    uint64_t  ino{0};                    ///< Inode number (filesystem-specific)
    uint64_t  size{0};                   ///< File size in bytes
    InodeType type{InodeType::Regular};  ///< Type of this inode
    InodeOps* ops{nullptr};              ///< Operation function table (may be nullptr)
    void*     fs_private{nullptr};       ///< Opaque pointer for filesystem-specific data

    uint32_t mode{0};    ///< File mode (type + permissions)
    uint32_t uid{0};     ///< Owner user ID
    uint32_t gid{0};     ///< Owner group ID
    uint32_t nlink{1};   ///< Hard link count
    uint64_t atime{0};   ///< Time of last access
    uint64_t ctime{0};   ///< Time of last status change
    uint64_t mtime{0};   ///< Time of last modification
    uint64_t blocks{0};  ///< Number of 512-byte blocks allocated
};

}  // namespace cinux::fs
