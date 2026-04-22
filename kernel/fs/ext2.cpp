/**
 * @file kernel/fs/ext2.cpp
 * @brief ext2 filesystem driver implementation
 *
 * Reads the ext2 superblock and block group descriptor table from disk
 * during mount(), then provides path-based lookup() and InodeOps for
 * reading files and listing directories.  All disk I/O goes through
 * the AHCI driver using DMA buffers allocated from PMM/VMM.
 */

#include "ext2.hpp"

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::fs {

// ============================================================
// Virtual address for ext2 DMA buffers
// ============================================================

/// Base virtual address for ext2 DMA page mappings
static constexpr uint64_t EXT2_DMA_VIRT_BASE = 0xFFFF800000400000ULL;

// ============================================================
// Ext2 InodeOps (static function pointer tables)
// ============================================================

namespace {

/**
 * @brief Read bytes from an ext2 file inode at a given offset
 *
 * Supports direct blocks (0-11) and singly-indirect block (12).
 * Reads through the ext2 DMA buffer, copying data to the caller's buffer.
 *
 * @param inode   The VFS inode to read from
 * @param offset  Byte offset within the file
 * @param buf     Destination buffer (kernel virtual address)
 * @param count   Number of bytes to read
 * @return Number of bytes actually read, or -1 on error
 */
int64_t ext2_file_read(const Inode* inode, uint64_t offset,
                       void* buf, uint64_t count);

/**
 * @brief Write bytes to an ext2 inode (read-only, always returns error)
 */
int64_t ext2_file_write(Inode*, uint64_t, const void*, uint64_t) {
    return -1;
}

/**
 * @brief Read the next directory entry name from an ext2 directory
 *
 * Scans directory data blocks, stepping through Ext2DirEntry records
 * by rec_len.  Returns "." for index 0, ".." for index 1, and
 * actual entries for index >= 2.
 *
 * @param inode      Directory VFS inode
 * @param index      Entry index (0-based)
 * @param name       Output buffer for the entry name
 * @param name_max   Size of the output buffer
 * @return 1 if an entry was read, 0 if no more entries, -1 on error
 */
int64_t ext2_dir_readdir(const Inode* inode, uint64_t index,
                         char* name, uint64_t name_max);

/// Static InodeOps for ext2 regular files
InodeOps ext2_file_ops = {
    ext2_file_read,
    ext2_file_write,
    nullptr,  // no readdir for regular files
};

/// Static InodeOps for ext2 directories
InodeOps ext2_dir_ops = {
    nullptr,           // no read on directories
    ext2_file_write,   // still returns -1
    ext2_dir_readdir,
};

// ============================================================
// Global ext2 instance pointer (set by Ext2 constructor)
// ============================================================

/// Pointer to the active Ext2 instance, used by InodeOps callbacks
static Ext2* g_ext2_instance = nullptr;

// ============================================================
// ext2_file_read implementation
// ============================================================

int64_t ext2_file_read(const Inode* inode, uint64_t offset,
                       void* buf, uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return -1;
    }

    auto* cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk = cached->disk_inode;

    // Bounds check
    if (offset >= disk.i_size) {
        return 0;
    }

    uint64_t available = disk.i_size - offset;
    uint64_t to_read = (count < available) ? count : available;

    if (to_read == 0) {
        return 0;
    }

    // We need access to the Ext2 instance for read_block
    Ext2* ext2 = g_ext2_instance;
    if (ext2 == nullptr) {
        return -1;
    }

    uint32_t bs = ext2->block_size();
    uint64_t block_ptrs_per_block = bs / sizeof(uint32_t);

    auto* dst = static_cast<uint8_t*>(buf);
    uint64_t total_read = 0;

    while (total_read < to_read) {
        // Which block does the current offset fall in?
        uint64_t file_block = (offset + total_read) / bs;
        uint64_t block_offset = (offset + total_read) % bs;
        uint64_t chunk = bs - block_offset;
        if (chunk > to_read - total_read) {
            chunk = to_read - total_read;
        }

        // Resolve file_block to a disk block number
        uint32_t disk_block = 0;

        if (file_block < EXT2_DIRECT_BLOCKS) {
            // Direct block
            disk_block = disk.i_block[file_block];
        } else if (file_block < EXT2_DIRECT_BLOCKS + block_ptrs_per_block) {
            // Singly-indirect block
            uint32_t indirect_block = disk.i_block[EXT2_INDIRECT_BLOCK];
            if (indirect_block == 0) {
                break;
            }

            // Read the indirect block to find the target
            if (!ext2->read_block(indirect_block)) {
                break;
            }

            uint32_t idx = static_cast<uint32_t>(
                file_block - EXT2_DIRECT_BLOCKS);
            auto* indirect = reinterpret_cast<uint32_t*>(
                ext2->dma_buf_virt());
            disk_block = indirect[idx];
        } else {
            // Doubly-indirect (not needed for small disks, but handle gracefully)
            // For simplicity, skip -- files on a 4MB disk should not need this
            break;
        }

        if (disk_block == 0) {
            // Sparse file: hole, return zeros
            for (uint64_t i = 0; i < chunk; ++i) {
                dst[total_read + i] = 0;
            }
            total_read += chunk;
            continue;
        }

        // Read the data block
        if (!ext2->read_block(disk_block)) {
            break;
        }

        auto* src = reinterpret_cast<const uint8_t*>(
            ext2->dma_buf_virt()) + block_offset;
        memcpy(dst + total_read, src, chunk);
        total_read += chunk;
    }

    return static_cast<int64_t>(total_read);
}

// ============================================================
// ext2_dir_readdir implementation
// ============================================================

int64_t ext2_dir_readdir(const Inode* inode, uint64_t index,
                         char* name, uint64_t name_max) {
    if (inode == nullptr || inode->fs_private == nullptr ||
        name == nullptr || name_max == 0) {
        return -1;
    }

    auto* cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk = cached->disk_inode;

    Ext2* ext2 = g_ext2_instance;
    if (ext2 == nullptr) {
        return -1;
    }

    uint32_t bs = ext2->block_size();

    // Special entries: "." and ".."
    if (index == 0) {
        if (name_max < 2) {
            return -1;
        }
        name[0] = '.';
        name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        if (name_max < 3) {
            return -1;
        }
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        return 1;
    }

    // Real entries: index-2 corresponds to the nth actual entry
    // We need to scan through directory data blocks and count entries
    uint64_t target = index - 2;
    uint64_t found = 0;
    uint64_t dir_size = disk.i_size;

    // Iterate over all data blocks of the directory
    uint32_t total_blocks = static_cast<uint32_t>(
        (dir_size + bs - 1) / bs);
    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;  // simple limit
    }

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!ext2->read_block(blk)) {
            return -1;
        }

        auto* block_data = reinterpret_cast<const uint8_t*>(
            ext2->dma_buf_virt());
        uint32_t pos = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<const Ext2DirEntry*>(
                block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0) {
                if (found == target) {
                    // Found the entry we want
                    uint32_t copy_len = static_cast<uint32_t>(name_max) - 1;
                    if (entry->name_len < copy_len) {
                        copy_len = entry->name_len;
                    }

                    for (uint32_t i = 0; i < copy_len; ++i) {
                        name[i] = entry->name[i];
                    }
                    name[copy_len] = '\0';
                    return 1;
                }
                ++found;
            }

            pos += entry->rec_len;
        }
    }

    return 0;  // no more entries
}

}  // anonymous namespace

// ============================================================
// Ext2 constructor
// ============================================================

Ext2::Ext2(cinux::drivers::ahci::AHCI& ahci, uint8_t port_index)
    : ahci_(ahci)
    , port_index_(port_index)
{
    // Register this instance as the active ext2 for InodeOps callbacks
    g_ext2_instance = this;
}

// ============================================================
// DMA buffer management
// ============================================================

bool Ext2::ensure_dma_buffer() {
    if (dma_ready_) {
        return true;
    }

    // Allocate a physical page for DMA
    dma_buf_phys_ = cinux::mm::g_pmm.alloc_page();
    if (dma_buf_phys_ == 0) {
        cinux::lib::kprintf("[EXT2] Failed to allocate DMA page\n");
        return false;
    }

    // Map it into kernel virtual address space
    constexpr uint64_t flags = cinux::arch::FLAG_PRESENT
                             | cinux::arch::FLAG_WRITABLE;
    dma_buf_virt_ = EXT2_DMA_VIRT_BASE;

    if (!cinux::mm::g_vmm.map(dma_buf_virt_, dma_buf_phys_, flags)) {
        cinux::lib::kprintf("[EXT2] Failed to map DMA page\n");
        cinux::mm::g_pmm.free_page(dma_buf_phys_);
        dma_buf_phys_ = 0;
        return false;
    }

    // Zero the buffer
    auto* buf = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    for (uint32_t i = 0; i < cinux::arch::PAGE_SIZE; ++i) {
        buf[i] = 0;
    }

    dma_ready_ = true;
    return true;
}

// ============================================================
// Block I/O
// ============================================================

bool Ext2::read_block(uint32_t block_num) {
    if (!ensure_dma_buffer()) {
        return false;
    }

    // Compute the LBA of the first sector of this block
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;

    // Read all sectors for this block
    bool ok = ahci_.read(port_index_, lba,
                         static_cast<uint16_t>(sectors_per_block_),
                         dma_buf_phys_);
    if (!ok) {
        cinux::lib::kprintf("[EXT2] read_block(%u) I/O failed\n", block_num);
    }
    return ok;
}

// ============================================================
// mount()
// ============================================================

bool Ext2::mount() {
    cinux::lib::kprintf("[EXT2] Mounting ext2 filesystem on AHCI port %u\n",
                        port_index_);

    // Step 1: Ensure DMA buffer is ready
    if (!ensure_dma_buffer()) {
        return false;
    }

    // Step 2: Read the superblock (at byte offset 1024 = LBA 2, 2 sectors)
    // The superblock is 1024 bytes starting at offset 1024.
    // For 512-byte sectors: LBA 2, count 2.
    constexpr uint64_t SB_LBA = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE;
    constexpr uint16_t SB_SECTORS = EXT2_SUPERBLOCK_SIZE / EXT2_SECTOR_SIZE;

    if (!ahci_.read(port_index_, SB_LBA, SB_SECTORS, dma_buf_phys_)) {
        cinux::lib::kprintf("[EXT2] Failed to read superblock\n");
        return false;
    }

    // Copy the superblock from the DMA buffer
    auto* dma = reinterpret_cast<const uint8_t*>(dma_buf_virt_);
    memcpy(&sb_, dma, sizeof(Ext2Superblock));

    // Step 3: Validate magic number
    if (sb_.s_magic != EXT2_SUPER_MAGIC) {
        cinux::lib::kprintf("[EXT2] Invalid magic: 0x%x (expected 0x%x)\n",
                            sb_.s_magic, EXT2_SUPER_MAGIC);
        return false;
    }

    // Step 4: Compute filesystem parameters
    block_size_ = 1024U << sb_.s_log_block_size;
    sectors_per_block_ = block_size_ / EXT2_SECTOR_SIZE;
    first_data_block_ = sb_.s_first_data_block;
    inode_size_ = (sb_.s_rev_level == 0)
        ? EXT2_INODE_SIZE_DEFAULT
        : sb_.s_inode_size;
    inodes_per_group_ = sb_.s_inodes_per_group;
    blocks_per_group_ = sb_.s_blocks_per_group;

    // Compute group count
    group_count_ = (sb_.s_blocks_count + blocks_per_group_ - 1)
        / blocks_per_group_;
    if (group_count_ > EXT2_MAX_GROUPS) {
        group_count_ = EXT2_MAX_GROUPS;
    }

    cinux::lib::kprintf("[EXT2] Superblock valid: magic=0x%x\n", sb_.s_magic);
    cinux::lib::kprintf("[EXT2]   block_size=%u  inode_size=%u\n",
                        block_size_, inode_size_);
    cinux::lib::kprintf("[EXT2]   blocks=%u  inodes=%u  groups=%u\n",
                        sb_.s_blocks_count, sb_.s_inodes_count, group_count_);
    cinux::lib::kprintf("[EXT2]   blocks_per_group=%u  inodes_per_group=%u\n",
                        blocks_per_group_, inodes_per_group_);

    // Step 5: Read the block group descriptor table
    // The BGDT starts at the block after the superblock block.
    // For 1K blocks: superblock is in block 1, BGDT starts at block 2.
    // For larger blocks: superblock is in block 0 (at offset 1024), BGDT at block 1.
    uint32_t bgdt_block = (block_size_ == 1024) ? 2 : 1;

    uint32_t bgdt_entries = group_count_;
    uint32_t bgdt_bytes = bgdt_entries * sizeof(Ext2BlockGroupDescriptor);
    uint32_t bgdt_blocks_needed = (bgdt_bytes + block_size_ - 1) / block_size_;

    for (uint32_t i = 0; i < bgdt_blocks_needed; ++i) {
        if (!read_block(bgdt_block + i)) {
            cinux::lib::kprintf("[EXT2] Failed to read BGDT block %u\n",
                                bgdt_block + i);
            return false;
        }

        auto* src = reinterpret_cast<const uint8_t*>(dma_buf_virt_);
        uint32_t entries_in_this_block = block_size_
            / sizeof(Ext2BlockGroupDescriptor);
        uint32_t start_entry = i * entries_in_this_block;
        uint32_t copy_count = entries_in_this_block;

        if (start_entry + copy_count > bgdt_entries) {
            copy_count = bgdt_entries - start_entry;
        }

        memcpy(&bgdt_[start_entry], src,
               copy_count * sizeof(Ext2BlockGroupDescriptor));
    }

    cinux::lib::kprintf("[EXT2] BGDT loaded: %u groups\n", group_count_);

    // Step 6: Set up the root directory inode (inode 2 in ext2)
    Ext2Inode root_disk;
    if (!read_disk_inode(2, root_disk)) {
        cinux::lib::kprintf("[EXT2] Failed to read root inode (ino=2)\n");
        return false;
    }

    // Place root inode in cache slot 0
    inode_cache_[0].ino = 2;
    inode_cache_[0].disk_inode = root_disk;
    inode_cache_[0].in_use = true;
    populate_vfs_inode(inode_cache_[0]);
    root_inode_ = inode_cache_[0].vfs_inode;

    cinux::lib::kprintf("[EXT2] Root inode: size=%u mode=0x%x\n",
                        root_disk.i_size, root_disk.i_mode);

    mounted_ = true;
    return true;
}

// ============================================================
// read_disk_inode()
// ============================================================

bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out_inode) {
    if (ino == 0) {
        return false;
    }

    // Compute block group
    uint32_t group = (ino - 1) / inodes_per_group_;

    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] Inode %u: group %u out of range\n",
                            ino, group);
        return false;
    }

    // Get the inode table start block from the group descriptor
    uint32_t inode_table_block = bgdt_[group].bg_inode_table;

    // Compute the index within the group
    uint32_t index_in_group = (ino - 1) % inodes_per_group_;

    // Compute byte offset within the inode table
    uint64_t byte_offset = static_cast<uint64_t>(index_in_group) * inode_size_;

    // Compute which block of the inode table contains this inode
    uint32_t block_offset = static_cast<uint32_t>(byte_offset / block_size_);
    uint32_t within_block_offset = static_cast<uint32_t>(
        byte_offset % block_size_);

    uint32_t target_block = inode_table_block + block_offset;

    // Read the block
    if (!read_block(target_block)) {
        cinux::lib::kprintf("[EXT2] Failed to read inode block %u\n",
                            target_block);
        return false;
    }

    // Extract the inode from the DMA buffer
    auto* block_data = reinterpret_cast<const uint8_t*>(dma_buf_virt_);

    // Safety: ensure we don't read past the block boundary
    if (within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] Inode %u crosses block boundary\n", ino);
        return false;
    }

    memcpy(&out_inode, block_data + within_block_offset, sizeof(Ext2Inode));
    return true;
}

// ============================================================
// Inode cache management
// ============================================================

Inode* Ext2::get_cached_inode(uint32_t ino) {
    // Search the cache for an existing entry
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (inode_cache_[i].in_use && inode_cache_[i].ino == ino) {
            return &inode_cache_[i].vfs_inode;
        }
    }

    // Find a free slot
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (!inode_cache_[i].in_use) {
            // Read the inode from disk
            if (!read_disk_inode(ino, inode_cache_[i].disk_inode)) {
                return nullptr;
            }

            inode_cache_[i].ino = ino;
            inode_cache_[i].in_use = true;
            populate_vfs_inode(inode_cache_[i]);
            return &inode_cache_[i].vfs_inode;
        }
    }

    // Cache full -- evict slot 1 (slot 0 is always root)
    // Simple FIFO eviction
    uint32_t evict = 1 + (ino % (EXT2_INODE_CACHE_SIZE - 1));

    inode_cache_[evict].in_use = false;
    if (!read_disk_inode(ino, inode_cache_[evict].disk_inode)) {
        return nullptr;
    }

    inode_cache_[evict].ino = ino;
    inode_cache_[evict].in_use = true;
    populate_vfs_inode(inode_cache_[evict]);
    return &inode_cache_[evict].vfs_inode;
}

void Ext2::populate_vfs_inode(Ext2CachedInode& cached) {
    const Ext2Inode& disk = cached.disk_inode;

    cached.vfs_inode.ino = cached.ino;

    // File size: for regular files in revision 0, i_size is the full size
    cached.vfs_inode.size = disk.i_size;

    // Determine type from i_mode
    uint16_t mode_type = disk.i_mode & EXT2_S_IFMT;
    if (mode_type == EXT2_S_IFDIR) {
        cached.vfs_inode.type = InodeType::Directory;
        cached.vfs_inode.ops = &ext2_dir_ops;
    } else if (mode_type == EXT2_S_IFREG) {
        cached.vfs_inode.type = InodeType::Regular;
        cached.vfs_inode.ops = &ext2_file_ops;
    } else {
        cached.vfs_inode.type = InodeType::Unknown;
        cached.vfs_inode.ops = nullptr;
    }

    cached.vfs_inode.fs_private = &cached;
}

// ============================================================
// lookup_in_dir()
// ============================================================

uint32_t Ext2::lookup_in_dir(uint32_t dir_ino, const char* name,
                             uint32_t name_len) {
    // Read the directory inode
    Ext2Inode dir_disk;
    if (!read_disk_inode(dir_ino, dir_disk)) {
        return 0;
    }

    uint32_t bs = block_size_;
    uint32_t dir_size = dir_disk.i_size;
    uint32_t total_blocks = (dir_size + bs - 1) / bs;

    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;
    }

    // Scan each data block of the directory
    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!read_block(blk)) {
            return 0;
        }

        auto* block_data = reinterpret_cast<const uint8_t*>(dma_buf_virt_);
        uint32_t pos = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<const Ext2DirEntry*>(
                block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0 &&
                entry->name_len == name_len) {
                // Compare names
                bool match = true;
                for (uint32_t i = 0; i < name_len; ++i) {
                    if (entry->name[i] != name[i]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    return entry->inode;
                }
            }

            pos += entry->rec_len;
        }
    }

    return 0;  // not found
}

// ============================================================
// lookup()
// ============================================================

Inode* Ext2::lookup(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }

    // Root directory
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return &root_inode_;
    }

    // Skip leading '/'
    if (path[0] == '/') {
        ++path;
    }

    // Walk the path component by component
    uint32_t current_ino = 2;  // start at root

    while (path[0] != '\0') {
        // Find the length of the current component
        uint32_t comp_len = 0;
        while (path[comp_len] != '\0' && path[comp_len] != '/') {
            ++comp_len;
        }

        if (comp_len == 0) {
            // Skip consecutive slashes
            ++path;
            continue;
        }

        // Look up the component in the current directory
        uint32_t found_ino = lookup_in_dir(current_ino, path, comp_len);
        if (found_ino == 0) {
            return nullptr;  // component not found
        }

        // Advance past this component
        path += comp_len;
        if (path[0] == '/') {
            ++path;
        }

        // If there are more components, the found inode must be a directory
        if (path[0] != '\0') {
            // Check that this inode is a directory
            Ext2Inode check;
            if (!read_disk_inode(found_ino, check)) {
                return nullptr;
            }
            if ((check.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
                return nullptr;  // intermediate component is not a directory
            }
        }

        current_ino = found_ino;
    }

    // Return the cached inode for the final component
    return get_cached_inode(current_ino);
}

// ============================================================
// Accessors
// ============================================================

uint32_t Ext2::block_size() const {
    return block_size_;
}

bool Ext2::is_mounted() const {
    return mounted_;
}

uint64_t Ext2::dma_buf_virt() const {
    return dma_buf_virt_;
}

}  // namespace cinux::fs
