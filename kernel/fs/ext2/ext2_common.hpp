/**
 * @file kernel/fs/ext2_common.hpp
 * @brief Shared ext2 constants and VFS InodeOps wrappers
 *
 * Houses the Ext2FileOps and Ext2DirOps classes that bridge the VFS
 * InodeOps interface to the concrete Ext2 driver.  Also defines the
 * EXT2_MAX_GROUPS constant shared across all ext2 sub-modules.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include "fs/inode.hpp"

namespace cinux::fs {

class Ext2;

/// Maximum block groups supported (covers up to ~8 GB with 4K blocks)
static constexpr uint32_t EXT2_MAX_GROUPS = 128;

/**
 * @brief InodeOps for ext2 regular files
 *
 * Overrides read() and write(); all other operations use the
 * InodeOps defaults (return -1 / nullptr).
 */
class Ext2FileOps : public InodeOps {
public:
    explicit Ext2FileOps(Ext2& ext2);

    cinux::lib::ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override;
    cinux::lib::ErrorOr<int64_t> write(Inode* inode, uint64_t offset, const void* buf, uint64_t count) override;
    cinux::lib::ErrorOr<void> stat(const Inode* inode, struct stat* st) override;

    /// ext2 regular files are disk-backed: route sys_read through the PageCache
    /// so repeated reads and demand paging share one copy of each page.
    bool is_page_cacheable() const override;

    // F-ECO batch 2: attribute + symlink read (file-target syscalls).
    cinux::lib::ErrorOr<void>    chmod(Inode* inode, uint32_t mode) override;
    cinux::lib::ErrorOr<void>    chown(Inode* inode, uint32_t uid, uint32_t gid) override;
    cinux::lib::ErrorOr<void>    utimensat(Inode* inode, uint64_t atime_sec, uint32_t atime_nsec,
                                           uint64_t mtime_sec, uint32_t mtime_nsec) override;
    cinux::lib::ErrorOr<int64_t> readlink(const Inode* inode, char* buf,
                                          uint64_t buf_size) override;

private:
    Ext2& ext2_;
};

/**
 * @brief InodeOps for ext2 directories
 *
 * Overrides readdir(), create(), mkdir(), and unlink();
 * read()/write() use the InodeOps defaults.
 */
class Ext2DirOps : public InodeOps {
public:
    explicit Ext2DirOps(Ext2& ext2);

    cinux::lib::ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name, uint64_t name_max) override;
    cinux::lib::ErrorOr<Inode*> create(Inode* dir, const char* name, uint32_t namelen) override;
    cinux::lib::ErrorOr<Inode*> mkdir(Inode* dir, const char* name, uint32_t namelen) override;
    cinux::lib::ErrorOr<void>   unlink(Inode* dir, const char* name, uint32_t namelen) override;
    cinux::lib::ErrorOr<void> stat(const Inode* inode, struct stat* st) override;

    // F-ECO batch 2: directory attribute + dirent ops. chmod/chown/utimensat act
    // on the directory inode itself; symlink/link/rename mutate its entries.
    cinux::lib::ErrorOr<void> chmod(Inode* inode, uint32_t mode) override;
    cinux::lib::ErrorOr<void> chown(Inode* inode, uint32_t uid, uint32_t gid) override;
    cinux::lib::ErrorOr<void> utimensat(Inode* inode, uint64_t atime_sec, uint32_t atime_nsec,
                                        uint64_t mtime_sec, uint32_t mtime_nsec) override;
    cinux::lib::ErrorOr<void> symlink(Inode* dir, const char* name, uint32_t namelen,
                                      const char* target) override;
    cinux::lib::ErrorOr<void> link(Inode* dir, const char* name, uint32_t namelen,
                                   const Inode* target) override;
    cinux::lib::ErrorOr<void> rename(Inode* src_dir, const char* src_name, uint32_t src_namelen,
                                     Inode* dst_dir, const char* dst_name,
                                     uint32_t dst_namelen) override;

private:
    Ext2& ext2_;
};

}  // namespace cinux::fs
