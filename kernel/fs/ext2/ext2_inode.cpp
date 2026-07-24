/**
 * @file kernel/fs/ext2_inode.cpp
 * @brief Ext2 inode read/write, allocation, caching, and block pointer resolution
 *
 * Handles reading and writing on-disk inodes, inode bitmap allocation
 * and deallocation, the fixed-size inode cache, VFS inode population,
 * and the get_or_alloc_block() block-pointer resolver.
 */

#include <stddef.h>
#include <stdint.h>

#include "ext2.hpp"
#include "kernel/fs/file.hpp"  // inode_ref / inode_unref (cache returns ref'd inodes)
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/proc/race_detect.hpp"  // F-DYN-COV: lockdep_assert_held (inode_cache_lock_ guard)

namespace cinux::fs {

// ============================================================
// Disk inode read/write
// ============================================================

bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out_inode) {
    if (ino == 0) {
        return false;
    }

    uint32_t group = (ino - 1) / inodes_per_group_;

    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] Inode %u: group %u out of range\n", ino, group);
        return false;
    }

    uint32_t inode_table_block = bgdt_[group].bg_inode_table;
    uint32_t index_in_group    = (ino - 1) % inodes_per_group_;
    uint64_t byte_offset       = static_cast<uint64_t>(index_in_group) * inode_size_;

    uint32_t block_offset        = static_cast<uint32_t>(byte_offset / block_size_);
    uint32_t within_block_offset = static_cast<uint32_t>(byte_offset % block_size_);

    uint32_t target_block = inode_table_block + block_offset;

    if (!read_block(target_block)) {
        cinux::lib::kprintf("[EXT2] Failed to read inode block %u\n", target_block);
        return false;
    }

    auto* block_data = reinterpret_cast<const uint8_t*>(block_buf_);

    if (within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] Inode %u crosses block boundary\n", ino);
        return false;
    }

    memcpy(&out_inode, block_data + within_block_offset, sizeof(Ext2Inode));
    return true;
}

bool Ext2::write_disk_inode(uint32_t ino, const Ext2Inode& inode) {
    if (ino == 0) {
        return false;
    }

    uint32_t group = (ino - 1) / inodes_per_group_;

    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: ino %u group %u out of range\n", ino, group);
        return false;
    }

    uint32_t inode_table_block   = bgdt_[group].bg_inode_table;
    uint32_t index_in_group      = (ino - 1) % inodes_per_group_;
    uint64_t byte_offset         = static_cast<uint64_t>(index_in_group) * inode_size_;
    uint32_t block_offset        = static_cast<uint32_t>(byte_offset / block_size_);
    uint32_t within_block_offset = static_cast<uint32_t>(byte_offset % block_size_);
    uint32_t target_block        = inode_table_block + block_offset;

    if (!read_block(target_block)) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to read block %u\n", target_block);
        return false;
    }

    if (within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: ino %u crosses block boundary\n", ino);
        return false;
    }

    auto* block_data = reinterpret_cast<uint8_t*>(block_buf_);
    memcpy(block_data + within_block_offset, &inode, sizeof(Ext2Inode));

    if (!write_block(target_block)) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to write block %u\n", target_block);
        return false;
    }

    return true;
}

// ============================================================
// Inode cache management
// ============================================================

Inode* Ext2::get_cached_inode(uint32_t ino) {
    // B3 inode cache race: inode_cache_ is shared across CPUs.  Serialize the
    // walk/evict/insert under inode_cache_lock_.  Held across read_disk_inode
    // (disk I/O) -- cache miss is rare and slow anyway; dropping the lock for
    // I/O would need a TOCTOU recheck.  lockdep_assert_held is the regression
    // guard if a future refactor drops the guard.
    auto g = inode_cache_lock_.guard();
    lockdep_assert_held(&inode_cache_lock_);
    if (ino == 0) {
        return nullptr;
    }

    // Separate-chaining hash: walk the ino % SIZE bucket chain for a hit.
    const uint32_t bucket = ino % EXT2_INODE_CACHE_SIZE;
    for (Ext2CachedInode* slot = inode_cache_[bucket]; slot != nullptr; slot = slot->hash_next) {
        if (slot->ino == ino) {
            // Hit.  If the on-disk inode was mutated out-of-band while still
            // referenced (refcount > 0), the entry was marked stale; refresh
            // the cached disk copy in place (same object, same address).  Then
            // take a reference for the caller -- the Linux inode model: lookup
            // returns a ref'd inode the caller must inode_unref().  This closes
            // the aliasing window the old code had (returning a refcount==0
            // pointer that eviction could silently repopulate for a different
            // ino under a live fd).
            if (slot->stale) {
                if (!read_disk_inode(ino, slot->disk_inode)) {
                    return nullptr;
                }
                populate_vfs_inode(*slot);
                slot->stale = false;
            }
            inode_ref(&slot->vfs_inode);
            return &slot->vfs_inode;
        }
    }

    // Miss.  Enforce the soft cap by freeing one UNREFERENCED object (any
    // bucket) to make room.  A live (refcount > 0) object is never freed --
    // that would reintroduce the aliasing UAF.  If every cached inode is in
    // use the cache is genuinely full; fail rather than corrupt.
    if (inode_cache_count_ >= EXT2_INODE_CACHE_MAX) {
        Ext2CachedInode** victim_prev = nullptr;
        for (uint32_t b = 0; b < EXT2_INODE_CACHE_SIZE && victim_prev == nullptr; ++b) {
            for (Ext2CachedInode** pp = &inode_cache_[b]; *pp != nullptr; pp = &(*pp)->hash_next) {
                if ((*pp)->vfs_inode.refcount == 0) {
                    victim_prev = pp;
                    break;
                }
            }
        }
        if (victim_prev == nullptr) {
            cinux::lib::kprintf(
                "[EXT2] inode cache at cap (%u) with all entries in use; cannot cache ino=%u\n",
                EXT2_INODE_CACHE_MAX, ino);
            return nullptr;
        }
        Ext2CachedInode* victim = *victim_prev;
        *victim_prev             = victim->hash_next;
        delete victim;
        --inode_cache_count_;
    }

    // Allocate a fresh heap object, read + populate, take a reference for the
    // caller, and insert at the bucket head.
    auto* obj = new Ext2CachedInode{};
    if (!read_disk_inode(ino, obj->disk_inode)) {
        delete obj;
        return nullptr;
    }
    obj->ino = ino;
    populate_vfs_inode(*obj);
    inode_ref(&obj->vfs_inode);  // caller owns this ref; pair with inode_unref
    obj->hash_next          = inode_cache_[bucket];
    inode_cache_[bucket]    = obj;
    ++inode_cache_count_;
    return &obj->vfs_inode;
}

void Ext2::populate_vfs_inode(Ext2CachedInode& cached) {
    const Ext2Inode& disk = cached.disk_inode;

    cached.vfs_inode.ino = cached.ino;

    cached.vfs_inode.size = disk.i_size;

    uint16_t mode_type = disk.i_mode & EXT2_S_IFMT;
    if (mode_type == EXT2_S_IFDIR) {
        cached.vfs_inode.type = InodeType::Directory;
        cached.vfs_inode.ops  = &dir_ops_;
    } else if (mode_type == EXT2_S_IFREG) {
        cached.vfs_inode.type = InodeType::Regular;
        cached.vfs_inode.ops  = &file_ops_;
    } else if (mode_type == EXT2_S_IFLNK) {
        // F-ECO batch 2 / F-USABILITY batch 1a: a symlink now has its own
        // InodeType (was Unknown -- no Symlink enum existed). readlink() yields
        // the target string: fast symlink inlines it in i_block[], long
        // symlink stores it in the i_block[0] data block. ops reuses file_ops_
        // so readlink() resolves.
        cached.vfs_inode.type = InodeType::Symlink;
        cached.vfs_inode.ops  = &file_ops_;
    } else {
        cached.vfs_inode.type = InodeType::Unknown;
        cached.vfs_inode.ops  = nullptr;
    }

    cached.vfs_inode.fs_private = &cached;

    cached.vfs_inode.mode   = disk.i_mode;
    cached.vfs_inode.uid    = disk.i_uid;
    cached.vfs_inode.gid    = disk.i_gid;
    cached.vfs_inode.nlink  = disk.i_links_count;
    cached.vfs_inode.atime  = disk.i_atime;
    cached.vfs_inode.ctime  = disk.i_ctime;
    cached.vfs_inode.mtime  = disk.i_mtime;
    cached.vfs_inode.blocks = disk.i_blocks;
}

// ============================================================
// Inode allocator
// ============================================================

uint32_t Ext2::alloc_inode() {
    if (!mounted_) {
        return 0;
    }

    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_inodes_count == 0) {
            continue;
        }

        uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
        if (bitmap_block == 0) {
            continue;
        }

        if (!read_block(bitmap_block)) {
            cinux::lib::kprintf("[EXT2] alloc_inode: failed to read bitmap block %u\n",
                                bitmap_block);
            return 0;
        }

        auto*    bitmap          = reinterpret_cast<uint8_t*>(block_buf_);
        uint32_t inodes_in_group = inodes_per_group_;
        uint32_t bytes_needed    = (inodes_in_group + 7) / 8;

        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (bitmap[byte_idx] == 0xFF) {
                continue;
            }

            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local_index = byte_idx * 8 + bit;
                if (local_index >= inodes_in_group) {
                    break;
                }

                if ((bitmap[byte_idx] & (1U << bit)) == 0) {
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);

                    if (!write_block(bitmap_block)) {
                        cinux::lib::kprintf("[EXT2] alloc_inode: failed to write bitmap\n");
                        return 0;
                    }

                    uint32_t global_ino = group * inodes_per_group_ + local_index + 1;

                    if (sb_.s_free_inodes_count > 0) {
                        --sb_.s_free_inodes_count;
                    }

                    if (bgdt_[group].bg_free_inodes_count > 0) {
                        --bgdt_[group].bg_free_inodes_count;
                    }

                    write_superblock();
                    write_bgdt(group);

                    return global_ino;
                }
            }
        }
    }

    cinux::lib::kprintf("[EXT2] alloc_inode: no free inodes available\n");
    return 0;
}

bool Ext2::free_inode(uint32_t ino) {
    if (ino == 0 || !mounted_) {
        return false;
    }

    uint32_t group = (ino - 1) / inodes_per_group_;
    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] free_inode: ino %u group out of range\n", ino);
        return false;
    }

    uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
    if (bitmap_block == 0) {
        return false;
    }

    uint32_t local_index = (ino - 1) % inodes_per_group_;

    if (!read_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_inode: failed to read bitmap block %u\n", bitmap_block);
        return false;
    }

    uint32_t byte_idx = local_index / 8;
    uint32_t bit      = local_index % 8;

    auto* bitmap = reinterpret_cast<uint8_t*>(block_buf_);
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    if (!write_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_inode: failed to write bitmap\n");
        return false;
    }

    ++sb_.s_free_inodes_count;
    ++bgdt_[group].bg_free_inodes_count;

    write_superblock();
    write_bgdt(group);

    return true;
}

// ============================================================
// Block pointer resolver with allocation
// ============================================================

uint32_t Ext2::get_or_alloc_block(Ext2Inode& disk, uint32_t file_block) {
    if (file_block < EXT2_DIRECT_BLOCKS) {
        if (disk.i_block[file_block] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(block_buf_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(blk)) {
                free_block(blk);
                return 0;
            }

            disk.i_block[file_block] = blk;
        }

        return disk.i_block[file_block];
    }

    if (file_block < EXT2_DIRECT_BLOCKS + block_size_ / sizeof(uint32_t)) {
        uint32_t indirect_idx = file_block - EXT2_DIRECT_BLOCKS;

        if (disk.i_block[EXT2_INDIRECT_BLOCK] == 0) {
            uint32_t indirect_blk = alloc_block();
            if (indirect_blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(block_buf_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(indirect_blk)) {
                free_block(indirect_blk);
                return 0;
            }

            disk.i_block[EXT2_INDIRECT_BLOCK] = indirect_blk;
        }

        uint32_t indirect_blk = disk.i_block[EXT2_INDIRECT_BLOCK];

        if (!read_block(indirect_blk)) {
            return 0;
        }

        auto* indirect = reinterpret_cast<uint32_t*>(block_buf_);

        if (indirect[indirect_idx] == 0) {
            uint32_t data_blk = alloc_block();
            if (data_blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(block_buf_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(data_blk)) {
                free_block(data_blk);
                return 0;
            }

            if (!read_block(indirect_blk)) {
                return 0;
            }
            indirect               = reinterpret_cast<uint32_t*>(block_buf_);
            indirect[indirect_idx] = data_blk;
            if (!write_block(indirect_blk)) {
                return 0;
            }
        }

        if (!read_block(indirect_blk)) {
            return 0;
        }
        indirect = reinterpret_cast<uint32_t*>(block_buf_);
        return indirect[indirect_idx];
    }

    // Doubly-indirect block: i_block[13] points at a block of ptrs_per_block
    // single-indirect pointers, each of which points at a block of
    // ptrs_per_block data pointers.  Logical-index layout in this region:
    //   offset = file_block - (EXT2_DIRECT_BLOCKS + ptrs_per_block)
    //   idx1   = offset / ptrs_per_block   -> slot in the double-indirect block
    //   idx2   = offset % ptrs_per_block   -> slot in the chosen indirect block
    //
    // Scratch-buffer discipline: block_buf_ is a single block-sized buffer, so
    // every write_block() clobbers whatever read_block() last loaded.  After
    // writing a freshly allocated child we must read_block() the parent back
    // before patching its pointer -- the same read-modify-write pattern the
    // single-indirect path uses above.
    const uint32_t ptrs_per_block = block_size_ / sizeof(uint32_t);
    const uint32_t di_base        = EXT2_DIRECT_BLOCKS + ptrs_per_block;
    const uint32_t di_limit       = di_base + ptrs_per_block * ptrs_per_block;
    if (file_block < di_limit) {
        const uint32_t offset = file_block - di_base;
        const uint32_t idx1   = offset / ptrs_per_block;
        const uint32_t idx2   = offset % ptrs_per_block;

        // Level 0: the double-indirect block itself (i_block[13]).
        if (disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK] == 0) {
            uint32_t di_blk = alloc_block();
            if (di_blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(block_buf_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(di_blk)) {
                free_block(di_blk);
                return 0;
            }

            disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK] = di_blk;
        }
        const uint32_t di_blk = disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK];

        // Level 1: the single-indirect child block at di_ptrs[idx1].
        if (!read_block(di_blk)) {
            return 0;
        }
        auto*    di_ptrs   = reinterpret_cast<uint32_t*>(block_buf_);
        uint32_t child_blk = di_ptrs[idx1];
        if (child_blk == 0) {
            child_blk = alloc_block();
            if (child_blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(block_buf_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(child_blk)) {
                free_block(child_blk);
                return 0;
            }
            // Re-read the double-indirect block: writing the child clobbered buf.
            if (!read_block(di_blk)) {
                return 0;
            }
            di_ptrs       = reinterpret_cast<uint32_t*>(block_buf_);
            di_ptrs[idx1] = child_blk;
            if (!write_block(di_blk)) {
                return 0;
            }
        }

        // Level 2: the data block at child_ptrs[idx2].
        if (!read_block(child_blk)) {
            return 0;
        }
        auto*    child_ptrs = reinterpret_cast<uint32_t*>(block_buf_);
        uint32_t data_blk   = child_ptrs[idx2];
        if (data_blk == 0) {
            data_blk = alloc_block();
            if (data_blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(block_buf_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(data_blk)) {
                free_block(data_blk);
                return 0;
            }
            // Re-read the child block: writing the data block clobbered buf.
            if (!read_block(child_blk)) {
                return 0;
            }
            child_ptrs       = reinterpret_cast<uint32_t*>(block_buf_);
            child_ptrs[idx2] = data_blk;
            if (!write_block(child_blk)) {
                return 0;
            }
        }

        return data_blk;
    }

    // Triple-indirect (i_block[14]) is intentionally unsupported: it would only
    // be reached for files beyond ~16 GB at 1 KB blocks, far past any current
    // use case for this driver.
    return 0;
}

}  // namespace cinux::fs
