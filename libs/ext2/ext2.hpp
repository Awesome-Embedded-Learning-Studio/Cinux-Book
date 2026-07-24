/**
 * @file kernel/fs/ext2.hpp
 * @brief ext2 filesystem driver (inherits from FileSystem)
 *
 * Implements the VFS FileSystem interface for ext2: mount(), lookup(), and
 * InodeOps (read, readdir) for files/dirs. Block I/O via IBlockDevice (e.g.
 * AHCIBlockDevice, owns DMA plumbing).
 *
 * Usage:
 *   cinux::fs::Ext2 ext2(block_dev);
 *   ext2.mount();
 *   vfs_mount_add("/", &ext2);
 *   Inode* ino = ext2.lookup("etc/motd");
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include "ext2_common.hpp"
#include "ext2_types.hpp"
#include "fs/vfs_filesystem.hpp"
#include "kernel/drivers/block_device.hpp"
#include "kernel/proc/sync.hpp"  // Spinlock (inode_cache_lock_)

namespace cinux::fs {

uint64_t ext2_read_count();   ///< cumulative ext2 read I/O (B2.5 dump_memory_stats)
uint64_t ext2_read_bytes();
uint64_t ext2_read_ns();

// ============================================================
// Ext2 Filesystem Driver Class
// ============================================================

/**
 * @brief ext2 filesystem driver (read-only)
 *
 * Reads the ext2 superblock and block group descriptor table from
 * disk during mount(), then provides path-based lookup() and
 * InodeOps for reading files and listing directories.
 *
 * Block I/O goes through a device-agnostic IBlockDevice; the backing device
 * (e.g. AHCIBlockDevice) owns any DMA plumbing.
 */
class Ext2 : public FileSystem {
public:
    /**
     * @brief Construct an ext2 driver over a block device
     *
     * @param dev  Block device backing the filesystem (e.g. AHCIBlockDevice).
     *             Must outlive the Ext2 instance.
     */
    explicit Ext2(cinux::drivers::IBlockDevice* dev);

    /// Destructor: frees every heap-owned cache object (singleton teardown).
    ~Ext2();

    /**
     * @brief Mount the ext2 filesystem
     *
     * Reads and validates the superblock, computes block_size,
     * reads the block group descriptor table, and prepares the
     * root directory inode for VFS lookup.
     *
     * @return ErrorOr<void> — Error::Ok on success, Error::IOError if the
     *         superblock is invalid or I/O fails
     */
    cinux::lib::ErrorOr<void> mount() override;

    /**
     * @brief Look up a file or directory by path
     *
     * The path is relative to the filesystem root (the mount layer
     * strips the mount prefix).  Performs component-by-component
     * traversal through directory entries.
     *
     * @param path  Null-terminated path relative to filesystem root
     * @return ErrorOr<Inode*> — a cached Inode on success, Error::NotFound if
     *         the path does not resolve, Error::IOError on read failure
     */
    cinux::lib::ErrorOr<Inode*> lookup(const char* path) override;

    /// F-USABILITY batch 1a: single-component lookup for the vfs_lookup layer.
    /// Resolves one name within parent via lookup_in_dir + get_cached_inode.
    cinux::lib::ErrorOr<Inode*> lookup_child(const Inode* parent, const char* name,
                                             uint32_t namelen) override;

    /**
     * @brief Get the resolved block size in bytes
     * @return Block size (1024, 2048, or 4096)
     */
    uint32_t block_size() const;

    /**
     * @brief Check whether the filesystem has been mounted
     * @return true if mount() succeeded
     */
    bool is_mounted() const;

    /// @brief Whether the volume advertises the ext4 extents incompat feature
    /// (s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS).  Per-inode reads
    /// still gate on EXT4_EXTENTS_FL; this surfaces the volume-level flag.
    bool has_ext4_extents_feature() const;

    /**
     * @brief Get the scratch block buffer (populated by read_block())
     *
     * Used by InodeOps callbacks to access block data after read_block(), or
     * to stage modified data before write_block().
     * @return Pointer to the block buffer (PAGE_SIZE bytes)
     */
    uint8_t* block_buf();

    /// Total on-disk blocks (s_blocks_count) -- bounds check for block pointers.
    uint32_t blocks_count() const { return sb_.s_blocks_count; }

    /// Read into block_buf_ (NOT SMP-safe; see dst overload).  @return true on success.
    bool read_block(uint32_t block_num);
    /// SMP-safe: read straight into @p dst (caller-provided).
    bool read_block(uint32_t block_num, void* dst);
    /// B3a: read @p block_count contiguous on-disk blocks straight into @p buf (one DMA,
    /// skipping block_buf_). Callers guarantee contiguity + block_count ≤ dma_buf (4 blk).
    cinux::lib::ErrorOr<void> read_disk_range(uint32_t start_disk_block, uint64_t block_count,
                                              void* buf);

    /**
     * @brief Write the block buffer contents back to an ext2 block on disk
     *
     * The caller should first populate the block buffer (via block_buf())
     * with the modified block data, then call write_block() to flush it.
     * This is the counterpart to read_block().  NOT SMP-safe (block_buf_); SMP
     * paths use write_block(b, src).
     */
    bool write_block(uint32_t block_num);
    /// SMP-safe: write @p src straight to disk (caller-provided).
    bool write_block(uint32_t block_num, void* src);

    /// Zero block_buf_ then write to @p blk.  NOT SMP-safe; SMP uses the src overload.
    bool zero_and_write_block(uint32_t blk);
    /// SMP-safe: zero @p src then write it.
    bool zero_and_write_block(uint32_t blk, void* src);

    /// Fill @p st from @p inode's cached on-disk fields.  Shared by Ext2FileOps
    /// and Ext2DirOps stat(): validates inputs, zeroes the struct (so the unset
    /// Linux-ABI fields stay 0), and copies the ext2 inode fields.
    cinux::lib::ErrorOr<void> fill_stat(const Inode* inode, struct stat* st) const;

    // ============================================================
    // File / directory mutation
    // ============================================================

    /**
     * @brief Create a new regular file inside a directory
     *
     * Allocates a new inode, initialises it as a regular file (mode REG,
     * links_count = 1), adds a directory entry in the parent, and writes
     * all modified metadata back to disk.
     *
     * @param parent_ino  Inode number of the parent directory
     * @param name        Name of the new file (NOT null-terminated)
     * @param name_len    Length of the name
     * @return Pointer to the new VFS Inode on success, nullptr on failure
     */
    Inode* create(uint32_t parent_ino, const char* name, uint32_t name_len);

    /**
     * @brief Create a new subdirectory inside a directory
     *
     * Allocates a new inode, initialises it as a directory (mode DIR,
     * links_count = 2), allocates one data block, writes "." and ".."
     * entries, adds a directory entry in the parent, and writes all
     * modified metadata back to disk.
     *
     * @param parent_ino  Inode number of the parent directory
     * @param name        Name of the new directory (NOT null-terminated)
     * @param name_len    Length of the name
     * @return Pointer to the new VFS Inode on success, nullptr on failure
     */
    Inode* mkdir(uint32_t parent_ino, const char* name, uint32_t name_len);

    /**
     * @brief Remove a directory entry and, if link count reaches zero,
     *        free the associated inode and data blocks
     *
     * Scans the parent directory for the named entry, removes it, and
     * decrements the target inode's link count.  If the link count
     * becomes zero, all data blocks are freed and the inode is released.
     *
     * @param parent_ino  Inode number of the parent directory
     * @param name        Name of the entry to remove (NOT null-terminated)
     * @param name_len    Length of the name
     * @return 0 on success, -1 on failure
     */
    int unlink(uint32_t parent_ino, const char* name, uint32_t name_len);

    // ============================================================
    // F-ECO batch 2: attribute + dirent operations
    // ============================================================

    /// Change an inode's permission bits (sys_chmod). Keeps the file-type bits
    /// in i_mode, replaces the low 12 permission bits. @return true on success.
    bool chmod(uint32_t ino, uint32_t mode);

    /// Change an inode's owner (sys_chown). 0xFFFFFFFF leaves uid/gid unchanged.
    bool chown(uint32_t ino, uint32_t uid, uint32_t gid);

    /// Set access/modification times (sys_utimensat). nsec truncated (rev-0 inode).
    bool utimensat(uint32_t ino, uint64_t atime_sec, uint32_t atime_nsec, uint64_t mtime_sec,
                   uint32_t mtime_nsec);

    /// Read a symlink's target into @p buf (sys_readlink).
    /// @return bytes written (>=0), or -1 on error.
    int64_t readlink(uint32_t ino, char* buf, uint64_t buf_size);

    /// Create a symbolic link (sys_symlink): new inode (S_IFLNK), target string
    /// stored in its first data block, linked into @p parent_ino.
    /// @return the new VFS Inode, or nullptr on failure.
    Inode* symlink(uint32_t parent_ino, const char* name, uint32_t name_len, const char* target);

    /// Create a hard link (sys_link): adds a directory entry in @p parent_ino
    /// referring to @p target_ino, and bumps the target's link count.
    bool link(uint32_t parent_ino, const char* name, uint32_t name_len, uint32_t target_ino);

    /// Rename (sys_rename): remove @p src_name from @p src_dir_ino and add
    /// @p dst_name referring to the same inode in @p dst_dir_ino. The two
    /// directories may be the same.
    bool rename(uint32_t src_dir_ino, const char* src_name, uint32_t src_len, uint32_t dst_dir_ino,
                const char* dst_name, uint32_t dst_len);

    // ============================================================
    // Block allocator
    // ============================================================

    /**
     * @brief Allocate a free data block from the filesystem
     *
     * Scans block bitmaps across all block groups to find a free block.
     * Marks the block as used in the bitmap, updates the superblock and
     * block group descriptor free-block counts, and writes all modified
     * metadata back to disk.
     *
     * @return Allocated block number (0-based), or 0 if the filesystem is full
     */
    uint32_t alloc_block();

    /**
     * @brief Release a previously allocated data block
     *
     * Clears the block's bit in the appropriate block bitmap, updates
     * the superblock and block group descriptor free-block counts, and
     * writes all modified metadata back to disk.
     *
     * @param block_num  ext2 block number to free (must be > 0)
     * @return true on success, false on error
     */
    bool free_block(uint32_t block_num);

    // ============================================================
    // Inode allocator
    // ============================================================

    /**
     * @brief Allocate a free inode from the filesystem
     *
     * Scans inode bitmaps across all block groups to find a free inode.
     * Marks the inode as used in the bitmap, updates the superblock and
     * block group descriptor free-inode counts, and writes all modified
     * metadata back to disk.
     *
     * @return Allocated inode number (1-based), or 0 if no free inodes
     */
    uint32_t alloc_inode();

    /**
     * @brief Release a previously allocated inode
     *
     * Clears the inode's bit in the appropriate inode bitmap, updates
     * the superblock and block group descriptor free-inode counts, and
     * writes all modified metadata back to disk.
     *
     * @param ino  Inode number to free (1-based, must be > 0)
     * @return true on success, false on error
     */
    bool free_inode(uint32_t ino);

    // ============================================================
    // Disk inode read/write (public for InodeOps callback access)
    // ============================================================

    /**
     * @brief Read an on-disk inode by inode number
     *
     * @param ino          Inode number (1-based)
     * @param out_inode    Output buffer for the inode data
     * @return true on success, false on I/O error
     */
    bool read_disk_inode(uint32_t ino, Ext2Inode& out_inode);

    /**
     * @brief Write an on-disk inode back to disk
     *
     * @param ino          Inode number (1-based)
     * @param inode        The inode data to write
     * @return true on success, false on I/O error
     */
    bool write_disk_inode(uint32_t ino, const Ext2Inode& inode);

    /**
     * @brief Get or allocate a block pointer for a given file block index
     *
     * @param disk      On-disk inode (modified with new block pointers)
     * @param file_block  Logical block index within the file (0..12)
     * @return Disk block number allocated, or 0 on failure
     */
    uint32_t get_or_alloc_block(Ext2Inode& disk, uint32_t file_block);

    /**
     * @brief Add a directory entry to a parent directory
     *
     * @param dir_ino      Parent directory inode number
     * @param dir_disk     On-disk inode (modified in-memory)
     * @param entry_ino    Inode number of the new entry
     * @param name         Entry name (NOT null-terminated)
     * @param name_len     Length of the name
     * @param file_type    Ext2FileType value for the new entry
     * @return true on success, false on failure
     */
    bool add_dir_entry(uint32_t dir_ino, Ext2Inode& dir_disk, uint32_t entry_ino, const char* name,
                       uint32_t name_len, Ext2FileType file_type);

    /**
     * @brief Remove a directory entry from a parent directory
     *
     * @param dir_ino      Parent directory inode number
     * @param dir_disk     On-disk inode of the parent directory
     * @param name         Entry name to remove (NOT null-terminated)
     * @param name_len     Length of the name
     * @param out_entry_ino  [out] Inode number of the removed entry
     * @return true on success, false on failure
     */
    bool remove_dir_entry(uint32_t dir_ino, const Ext2Inode& dir_disk, const char* name,
                          uint32_t name_len, uint32_t& out_entry_ino);

private:
    /// Located inode (block + byte offset within it); filled by locate_inode_block.
    struct InodeLoc {
        uint32_t target_block;
        uint32_t within_block_offset;
    };

    /// Locate @p ino's containing block + offset, read_block() it, bounds-check.
    /// Shared by read_disk_inode / write_disk_inode (the inode-location math is
    /// identical).  On success block_buf_ holds ino's block; @return true.
    bool locate_inode_block(uint32_t ino, InodeLoc& out);

    // ============================================================
    // Metadata write-back helpers
    // ============================================================

    /**
     * @brief Write the cached superblock back to disk
     *
     * Flushes the in-memory sb_ to the on-disk superblock location
     * (byte offset 1024).
     *
     * @return true on success, false on I/O error
     */
    bool write_superblock();

    /**
     * @brief Write a single block group descriptor back to disk
     *
     * Reads the BGDT block that contains the specified group's descriptor,
     * patches the descriptor entry, and writes the block back.
     *
     * @param group  Block group index (0-based)
     * @return true on success, false on I/O error
     */
    bool write_bgdt(uint32_t group);

    // ============================================================
    // Inode cache management
    // ============================================================

    /**
     * @brief Find or allocate a cache slot for the given inode number
     *
     * If the inode is already cached, returns its slot.  Otherwise
     * finds a free slot, reads the inode from disk, and populates
     * both the disk_inode and vfs_inode fields.
     *
     * @param ino  Inode number (1-based)
     * @return Pointer to the VFS Inode, or nullptr on failure
     */
    Inode* get_cached_inode(uint32_t ino);

    /**
     * @brief Build a VFS Inode from an on-disk ext2 inode
     *
     * Populates the vfs_inode fields (ino, size, type, ops, fs_private)
     * based on the disk inode contents.
     *
     * @param cached  Cache entry to populate
     */
    void populate_vfs_inode(Ext2CachedInode& cached);

    /// Drop the cached entry for @p ino (if present) so the next lookup re-reads
    /// disk. Call after a metadata write (chmod/chown/utimensat/link): stat()
    /// serves the cached disk_inode, so a write that skips this stays stale
    /// until the slot is evicted.
    void invalidate_cached_inode(uint32_t ino);

    // ============================================================
    // Path resolution
    // ============================================================

    /**
     * @brief Look up a single component name inside a directory inode
     *
     * Scans the directory data blocks for an entry whose name matches
     * the given component.  Returns the matching entry's inode number.
     *
     * @param dir_ino      Inode number of the directory to search
     * @param name         Component name to find (null-terminated)
     * @param name_len     Length of the name
     * @return Inode number of the matching entry, or 0 if not found
     */
    uint32_t lookup_in_dir(uint32_t dir_ino, const char* name, uint32_t name_len);

    // ============================================================
    // Member data
    // ============================================================

    /// Ops instances for file and directory inodes
    Ext2FileOps file_ops_;
    Ext2DirOps  dir_ops_;

    /// Block device backing the filesystem (not owned)
    cinux::drivers::IBlockDevice* dev_;

    /// Scratch block buffer for read_block()/write_block() (max ext2 block = 4096 B)
    uint8_t block_buf_[4096];

    /// Snapshot buffers for unlink()'s indirect-block release.  free_block()
    /// does its own read_block(bitmap)+write_block(bitmap), which overwrite
    /// block_buf_; so unlink must copy each indirect pointer array out of
    /// block_buf_ BEFORE freeing the data blocks it lists -- otherwise every
    /// entry after the first free_block() reads bitmap bytes reinterpreted as
    /// a block number (the "group out of range" garbage seen when a file that
    /// spans indirect blocks is unlinked).  Two buffers because the doubly-
    /// indirect walk is nested: the top-level array must survive while each
    /// child's array is processed, so they cannot share one buffer.
    uint32_t unlink_ptr_buf_[1024];
    uint32_t unlink_child_buf_[1024];

    /// Whether mount() has succeeded
    bool mounted_{};

    /// Computed block size in bytes (1024, 2048, or 4096)
    uint32_t block_size_{};

    /// Number of sectors per ext2 block
    uint32_t sectors_per_block_{};

    /// First data block number (1 for 1K blocks, 0 otherwise)
    uint32_t first_data_block_{};

    /// Inode size in bytes (from superblock)
    uint16_t inode_size_{};

    /// Number of inodes per block group
    uint32_t inodes_per_group_{};

    /// Number of blocks per block group
    uint32_t blocks_per_group_{};

    /// Total number of block groups
    uint32_t group_count_{};

    /// Superblock (cached after mount)
    Ext2Superblock sb_{};

    /// Block group descriptor table (cached after mount)
    Ext2BlockGroupDescriptor bgdt_[EXT2_MAX_GROUPS]{};

    /// Inode cache: separate-chaining hash (bucket = ino % SIZE) of heap-owned
    /// Ext2CachedInode.  Live (refcount > 0) objects are never moved/repopulated.
    Ext2CachedInode* inode_cache_[EXT2_INODE_CACHE_SIZE]{};

    /// Live object count; capped at EXT2_INODE_CACHE_MAX (evict refcount==0).
    uint32_t inode_cache_count_{0};
    mutable cinux::proc::Spinlock inode_cache_lock_;  ///< SMP: serialize cache walks/evicts
    mutable cinux::proc::Spinlock block_alloc_lock_;  ///< SMP: serialize block+inode bitmap alloc/free
};

}  // namespace cinux::fs
