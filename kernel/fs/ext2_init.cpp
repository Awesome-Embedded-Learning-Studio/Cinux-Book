/**
 * @file kernel/fs/ext2_init.cpp
 * @brief Ext2 initialisation, mount, block I/O, and path resolution
 *
 * Contains the Ext2 constructor, DMA buffer setup, mount sequence,
 * low-level block read/write, superblock/BGDT write-back, accessor
 * methods, and path-component resolution (lookup_in_dir, lookup).
 */

#include <stddef.h>
#include <stdint.h>

#include "ext2.hpp"
#include "kernel/drivers/block_device.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::fs {

// ============================================================
// Constructor
// ============================================================

Ext2::Ext2(cinux::drivers::IBlockDevice* dev) : file_ops_(*this), dir_ops_(*this), dev_(dev) {}

// ============================================================
// Block I/O
// ============================================================

bool Ext2::read_block(uint32_t block_num) {
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;

    auto r = dev_->read_blocks(lba, sectors_per_block_, block_buf_);
    if (!r.ok()) {
        cinux::lib::kprintf("[EXT2] read_block(%u) I/O failed\n", block_num);
        return false;
    }
    return true;
}

bool Ext2::write_block(uint32_t block_num) {
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;

    auto r = dev_->write_blocks(lba, sectors_per_block_, block_buf_);
    if (!r.ok()) {
        cinux::lib::kprintf("[EXT2] write_block(%u) I/O failed\n", block_num);
        return false;
    }
    return true;
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

uint8_t* Ext2::block_buf() {
    return block_buf_;
}

// ============================================================
// mount()
// ============================================================

cinux::lib::ErrorOr<void> Ext2::mount() {
    cinux::lib::kprintf("[EXT2] Mounting ext2 filesystem\n");

    // Read the superblock (byte offset 1024 = LBA 2, 2 sectors)
    constexpr uint64_t SB_LBA     = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE;
    constexpr uint16_t SB_SECTORS = EXT2_SUPERBLOCK_SIZE / EXT2_SECTOR_SIZE;

    if (!dev_->read_blocks(SB_LBA, SB_SECTORS, block_buf_).ok()) {
        cinux::lib::kprintf("[EXT2] Failed to read superblock\n");
        return cinux::lib::Error::IOError;
    }

    memcpy(&sb_, block_buf_, sizeof(Ext2Superblock));

    if (sb_.s_magic != EXT2_SUPER_MAGIC) {
        cinux::lib::kprintf("[EXT2] Invalid magic: 0x%x (expected 0x%x)\n", sb_.s_magic,
                            EXT2_SUPER_MAGIC);
        return cinux::lib::Error::IOError;
    }

    // Compute filesystem parameters
    block_size_        = 1024U << sb_.s_log_block_size;
    sectors_per_block_ = block_size_ / EXT2_SECTOR_SIZE;
    first_data_block_  = sb_.s_first_data_block;
    inode_size_        = (sb_.s_rev_level == 0) ? EXT2_INODE_SIZE_DEFAULT : sb_.s_inode_size;
    inodes_per_group_  = sb_.s_inodes_per_group;
    blocks_per_group_  = sb_.s_blocks_per_group;

    group_count_ = (sb_.s_blocks_count + blocks_per_group_ - 1) / blocks_per_group_;
    if (group_count_ > EXT2_MAX_GROUPS) {
        group_count_ = EXT2_MAX_GROUPS;
    }

    cinux::lib::kprintf("[EXT2] Superblock valid: magic=0x%x\n", sb_.s_magic);
    cinux::lib::kprintf("[EXT2]   block_size=%u  inode_size=%u\n", block_size_, inode_size_);
    cinux::lib::kprintf("[EXT2]   blocks=%u  inodes=%u  groups=%u\n", sb_.s_blocks_count,
                        sb_.s_inodes_count, group_count_);
    cinux::lib::kprintf("[EXT2]   blocks_per_group=%u  inodes_per_group=%u\n", blocks_per_group_,
                        inodes_per_group_);

    // Read the block group descriptor table
    uint32_t bgdt_block = (block_size_ == 1024) ? 2 : 1;

    uint32_t bgdt_entries       = group_count_;
    uint32_t bgdt_bytes         = bgdt_entries * sizeof(Ext2BlockGroupDescriptor);
    uint32_t bgdt_blocks_needed = (bgdt_bytes + block_size_ - 1) / block_size_;

    for (uint32_t i = 0; i < bgdt_blocks_needed; ++i) {
        if (!read_block(bgdt_block + i)) {
            cinux::lib::kprintf("[EXT2] Failed to read BGDT block %u\n", bgdt_block + i);
            return cinux::lib::Error::IOError;
        }

        auto*    src                   = block_buf_;
        uint32_t entries_in_this_block = block_size_ / sizeof(Ext2BlockGroupDescriptor);
        uint32_t start_entry           = i * entries_in_this_block;
        uint32_t copy_count            = entries_in_this_block;

        if (start_entry + copy_count > bgdt_entries) {
            copy_count = bgdt_entries - start_entry;
        }

        memcpy(&bgdt_[start_entry], src, copy_count * sizeof(Ext2BlockGroupDescriptor));
    }

    cinux::lib::kprintf("[EXT2] BGDT loaded: %u groups\n", group_count_);

    // Set up the root directory inode (inode 2 in ext2)
    Ext2Inode root_disk;
    if (!read_disk_inode(2, root_disk)) {
        cinux::lib::kprintf("[EXT2] Failed to read root inode (ino=2)\n");
        return cinux::lib::Error::IOError;
    }

    inode_cache_[0].ino        = 2;
    inode_cache_[0].disk_inode = root_disk;
    inode_cache_[0].in_use     = true;
    populate_vfs_inode(inode_cache_[0]);
    root_inode_ = inode_cache_[0].vfs_inode;

    cinux::lib::kprintf("[EXT2] Root inode: size=%u mode=0x%x\n", root_disk.i_size,
                        root_disk.i_mode);

    mounted_ = true;
    return {};
}

// ============================================================
// Superblock / BGDT write-back
// ============================================================

bool Ext2::write_superblock() {
    constexpr uint64_t SB_LBA     = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE;
    constexpr uint16_t SB_SECTORS = EXT2_SUPERBLOCK_SIZE / EXT2_SECTOR_SIZE;

    memcpy(block_buf_, &sb_, sizeof(Ext2Superblock));

    if (!dev_->write_blocks(SB_LBA, SB_SECTORS, block_buf_).ok()) {
        cinux::lib::kprintf("[EXT2] write_superblock: I/O failed\n");
        return false;
    }

    return true;
}

bool Ext2::write_bgdt(uint32_t group) {
    if (group >= group_count_) {
        return false;
    }

    uint32_t bgdt_start_block  = (block_size_ == 1024) ? 2 : 1;
    uint32_t entries_per_block = block_size_ / sizeof(Ext2BlockGroupDescriptor);
    uint32_t bgdt_block_index  = group / entries_per_block;
    uint32_t entry_in_block    = group % entries_per_block;
    uint32_t disk_block        = bgdt_start_block + bgdt_block_index;

    if (!read_block(disk_block)) {
        cinux::lib::kprintf("[EXT2] write_bgdt: failed to read block %u\n", disk_block);
        return false;
    }

    auto* block_data = block_buf_;
    memcpy(block_data + entry_in_block * sizeof(Ext2BlockGroupDescriptor), &bgdt_[group],
           sizeof(Ext2BlockGroupDescriptor));

    if (!write_block(disk_block)) {
        cinux::lib::kprintf("[EXT2] write_bgdt: failed to write block %u\n", disk_block);
        return false;
    }

    return true;
}

// ============================================================
// Path resolution
// ============================================================

uint32_t Ext2::lookup_in_dir(uint32_t dir_ino, const char* name, uint32_t name_len) {
    Ext2Inode dir_disk;
    if (!read_disk_inode(dir_ino, dir_disk)) {
        return 0;
    }

    uint32_t bs           = block_size_;
    uint32_t dir_size     = dir_disk.i_size;
    uint32_t total_blocks = (dir_size + bs - 1) / bs;

    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;
    }

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!read_block(blk)) {
            return 0;
        }

        auto*    block_data = block_buf_;
        uint32_t pos        = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<const Ext2DirEntry*>(block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0 && entry->name_len == name_len) {
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

    return 0;
}

cinux::lib::ErrorOr<Inode*> Ext2::lookup(const char* path) {
    if (path == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return &root_inode_;
    }

    if (path[0] == '/') {
        ++path;
    }

    uint32_t current_ino = 2;

    while (path[0] != '\0') {
        uint32_t comp_len = 0;
        while (path[comp_len] != '\0' && path[comp_len] != '/') {
            ++comp_len;
        }

        if (comp_len == 0) {
            ++path;
            continue;
        }

        uint32_t found_ino = lookup_in_dir(current_ino, path, comp_len);
        if (found_ino == 0) {
            return cinux::lib::Error::NotFound;
        }

        path += comp_len;
        if (path[0] == '/') {
            ++path;
        }

        if (path[0] != '\0') {
            Ext2Inode check;
            if (!read_disk_inode(found_ino, check)) {
                return cinux::lib::Error::IOError;
            }
            if ((check.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
                return cinux::lib::Error::NotFound;
            }
        }

        current_ino = found_ino;
    }

    Inode* result = get_cached_inode(current_ino);
    if (result == nullptr) {
        return cinux::lib::Error::IOError;
    }
    return result;
}

}  // namespace cinux::fs
