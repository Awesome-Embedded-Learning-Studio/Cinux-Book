/**
 * @file kernel/fs/ext2.hpp
 * @brief ext2 filesystem driver (inherits from FileSystem)
 *
 * Implements the VFS FileSystem interface for the ext2 filesystem.
 * Reads blocks from disk via the AHCI driver and DMA buffers
 * allocated through PMM/VMM.  Supports mount(), lookup(), and
 * InodeOps (read, readdir) for files and directories.
 *
 * Usage:
 *   cinux::fs::Ext2 ext2(ahci, port_index);
 *   ext2.mount();
 *   vfs_mount_add("/", &ext2);
 *   Inode* ino = ext2.lookup("etc/motd");
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "fs/ext2_types.hpp"
#include "fs/vfs_filesystem.hpp"

namespace cinux::drivers::ahci {
class AHCI;
}

namespace cinux::fs {

// ============================================================
// Maximum number of block group descriptors
// ============================================================

/// Maximum block groups supported (covers up to ~8 GB with 4K blocks)
static constexpr uint32_t EXT2_MAX_GROUPS = 128;

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
 * Block I/O is performed through the AHCI driver using DMA buffers
 * allocated from the PMM and mapped into kernel virtual address space.
 */
class Ext2 : public FileSystem {
public:
    /**
     * @brief Construct an ext2 driver bound to an AHCI port
     *
     * @param ahci         Reference to the initialised AHCI controller
     * @param port_index   AHCI port number where the ext2 disk resides
     */
    Ext2(cinux::drivers::ahci::AHCI& ahci, uint8_t port_index);

    /**
     * @brief Mount the ext2 filesystem
     *
     * Reads and validates the superblock, computes block_size,
     * reads the block group descriptor table, and prepares the
     * root directory inode for VFS lookup.
     *
     * @return true on success, false if superblock is invalid or I/O fails
     */
    bool mount() override;

    /**
     * @brief Look up a file or directory by path
     *
     * The path is relative to the filesystem root (the mount layer
     * strips the mount prefix).  Performs component-by-component
     * traversal through directory entries.
     *
     * @param path  Null-terminated path relative to filesystem root
     * @return Pointer to a cached Inode, or nullptr if not found
     */
    Inode* lookup(const char* path) override;

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

    /**
     * @brief Get the virtual address of the DMA buffer (read-only access)
     *
     * Used by InodeOps callbacks to access block data after read_block().
     * @return Virtual address of the DMA buffer
     */
    uint64_t dma_buf_virt() const;

    /**
     * @brief Read an ext2 block from disk into the DMA buffer
     *
     * Public wrapper used by InodeOps callbacks.
     *
     * @param block_num  ext2 block number (0-based)
     * @return true on success, false on I/O error
     */
    bool read_block(uint32_t block_num);

private:
    // ============================================================
    // Disk I/O helpers
    // ============================================================

    /**
     * @brief Ensure the DMA buffer is allocated and mapped
     *
     * Allocates a physical page and maps it into kernel virtual
     * address space if not already done.  Called lazily by
     * read_block() on first use.
     *
     * @return true if the buffer is ready, false on allocation failure
     */
    bool ensure_dma_buffer();

    /**
     * @brief Read an on-disk inode by inode number
     *
     * Computes the block group, locates the inode table, reads
     * the appropriate block, and copies the inode data into
     * the provided buffer.
     *
     * @param ino          Inode number (1-based)
     * @param out_inode    Output buffer for the inode data
     * @return true on success, false on I/O error
     */
    bool read_disk_inode(uint32_t ino, Ext2Inode& out_inode);

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

    /// Reference to the AHCI controller
    cinux::drivers::ahci::AHCI& ahci_;

    /// AHCI port index where the ext2 disk is attached
    uint8_t port_index_;

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

    /// Inode cache (fixed-size array)
    Ext2CachedInode inode_cache_[EXT2_INODE_CACHE_SIZE]{};

    /// Root directory VFS inode (always at cache slot 0 after mount)
    Inode root_inode_{};

    /// Physical address of the single-block DMA buffer
    uint64_t dma_buf_phys_{};

    /// Virtual address of the single-block DMA buffer
    uint64_t dma_buf_virt_{};

    /// Whether the DMA buffer has been allocated and mapped
    bool dma_ready_{};
};

}  // namespace cinux::fs
