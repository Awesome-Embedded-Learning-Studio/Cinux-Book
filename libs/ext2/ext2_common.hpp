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

#include "ext2_types.hpp"
#include "fs/inode.hpp"
#include "kernel/mm/slab.hpp"  // kmalloc/kfree (KmBuf)

namespace cinux::fs {

class Ext2;

/// RAII kmalloc'd scratch buffer for ext2 SMP read/write paths (block_buf_ is
/// shared/non-thread-safe). Heap not stack: demand-page #PF runs on IST2 which
/// is only 4 KB (IRQ_STACK_PAGES=1). ~KmBuf kfrees; operator bool checks OOM.
class KmBuf {
    void* p_;
public:
    explicit KmBuf(uint64_t n) : p_(cinux::mm::kmalloc(n, 16)) {}
    ~KmBuf() {
        if (p_ != nullptr) {
            cinux::mm::kfree(p_);
        }
    }
    KmBuf(const KmBuf&)            = delete;
    KmBuf& operator=(const KmBuf&) = delete;
    explicit operator bool() const { return p_ != nullptr; }
    void*    get() const { return p_; }
    uint8_t* data() const { return static_cast<uint8_t*>(p_); }
};

/// ext2-private typed accessor: Inode::fs_private holds the Ext2CachedInode.
/// Centralises the downcast so a typo'd mask or const-wrong variant can't
/// spread across the InodeOps call sites.
inline Ext2CachedInode* ext2_cached_inode(Inode* inode) {
    return static_cast<Ext2CachedInode*>(inode->fs_private);
}
inline const Ext2CachedInode* ext2_cached_inode(const Inode* inode) {
    return static_cast<const Ext2CachedInode*>(inode->fs_private);
}

/// @brief Does this dirent's name equal @p name (length + bytes)?
/// Centralises the byte-compare both directory.cpp (remove-dir-entry) and
/// init.cpp (inode-by-name) do while walking a directory block.
inline bool dirent_name_matches(const Ext2DirEntry& entry, const char* name, uint32_t name_len) {
    if (entry.name_len != name_len) {
        return false;
    }
    for (uint32_t i = 0; i < name_len; ++i) {
        if (entry.name[i] != name[i]) {
            return false;
        }
    }
    return true;
}

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

    cinux::lib::ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf,
                                      uint64_t count) override;
    cinux::lib::ErrorOr<int64_t> write(Inode* inode, uint64_t offset, const void* buf,
                                       uint64_t count) override;
    cinux::lib::ErrorOr<void>    stat(const Inode* inode, struct stat* st) override;
    cinux::lib::ErrorOr<void>    truncate(Inode* inode, uint64_t new_size) override;

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

    /// B3a: resolve @p file_block → on-disk block (0 = hole / unmapped / I/O error /
    /// extent-unsupported). Extracted from read() so coalescing can probe N contiguous
    /// blocks. Uses @p scratch (caller-provided, SMP-safe) for indirect-table reads
    /// instead of the shared block_buf_; data I/O uses read_disk_range.
    uint32_t resolve_disk_block_(const Ext2Inode& disk, uint64_t file_block,
                                 uint64_t block_ptrs_per_block, uint8_t* scratch);
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

    cinux::lib::ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name,
                                         uint64_t name_max) override;
    cinux::lib::ErrorOr<Inode*>  create(Inode* dir, const char* name, uint32_t namelen) override;
    cinux::lib::ErrorOr<Inode*>  mkdir(Inode* dir, const char* name, uint32_t namelen) override;
    cinux::lib::ErrorOr<void>    unlink(Inode* dir, const char* name, uint32_t namelen) override;
    cinux::lib::ErrorOr<void>    stat(const Inode* inode, struct stat* st) override;

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
