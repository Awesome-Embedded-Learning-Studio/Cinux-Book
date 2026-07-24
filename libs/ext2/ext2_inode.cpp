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

bool Ext2::locate_inode_block(uint32_t ino, InodeLoc& out) {
    if (ino == 0) {
        return false;
    }
    uint32_t group = (ino - 1) / inodes_per_group_;
    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] inode %u: group %u out of range\n", ino, group);
        return false;
    }
    // Inode byte offset within its group's inode table.
    const uint64_t byte_offset = static_cast<uint64_t>((ino - 1) % inodes_per_group_) * inode_size_;
    out.target_block        = bgdt_[group].bg_inode_table + static_cast<uint32_t>(byte_offset / block_size_);
    out.within_block_offset = static_cast<uint32_t>(byte_offset % block_size_);
    // Pure arithmetic bounds check; the block read is the caller's job
    // (read_disk_inode / write_disk_inode each use their own KmBuf so two
    // CPUs touching different inodes don't share block_buf_).
    if (out.within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] inode %u crosses block boundary\n", ino);
        return false;
    }
    return true;
}

bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out_inode) {
    InodeLoc loc{};
    if (!locate_inode_block(ino, loc)) {
        return false;
    }
    KmBuf buf(4096);
    if (!buf) {
        return false;
    }
    if (!read_block(loc.target_block, buf.get())) {
        cinux::lib::kprintf("[EXT2] failed to read inode block %u\n", loc.target_block);
        return false;
    }
    memcpy(&out_inode, buf.data() + loc.within_block_offset, sizeof(Ext2Inode));
    return true;
}

bool Ext2::write_disk_inode(uint32_t ino, const Ext2Inode& inode) {
    InodeLoc loc{};
    if (!locate_inode_block(ino, loc)) {
        return false;
    }
    // Read-modify-write: load the containing block, patch the inode slot,
    // write the block back.  Each call uses its own KmBuf so concurrent
    // inode writes on different CPUs don't share block_buf_.
    KmBuf buf(4096);
    if (!buf) {
        return false;
    }
    if (!read_block(loc.target_block, buf.get())) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to read block %u\n", loc.target_block);
        return false;
    }
    memcpy(buf.data() + loc.within_block_offset, &inode, sizeof(Ext2Inode));
    if (!write_block(loc.target_block, buf.get())) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to write block %u\n", loc.target_block);
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

// populate_vfs_inode() lives in ext2_metadata.cpp (split for the 500-line
// limit): it shares the disk->VFS inode translation done there.

// ============================================================
// Inode allocator
// ============================================================

uint32_t Ext2::alloc_inode() {
    // SMP: serialize inode bitmap alloc vs block alloc / other inode allocs
    // (shared block_buf_ in write_superblock/write_bgdt + bitmap RMW race).
    auto g = block_alloc_lock_.guard();
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

        KmBuf   bitmap_buf(4096);
        if (!bitmap_buf) {
            return 0;
        }
        if (!read_block(bitmap_block, bitmap_buf.get())) {
            cinux::lib::kprintf("[EXT2] alloc_inode: failed to read bitmap block %u\n",
                                bitmap_block);
            return 0;
        }

        auto*    bitmap          = bitmap_buf.data();
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

                    if (!write_block(bitmap_block, bitmap_buf.get())) {
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
    // SMP: serialize inode bitmap free (see alloc_inode above).
    auto g = block_alloc_lock_.guard();
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

    KmBuf bitmap_buf(4096);
    if (!bitmap_buf) {
        return false;
    }
    if (!read_block(bitmap_block, bitmap_buf.get())) {
        cinux::lib::kprintf("[EXT2] free_inode: failed to read bitmap block %u\n", bitmap_block);
        return false;
    }

    uint32_t byte_idx = local_index / 8;
    uint32_t bit      = local_index % 8;

    auto* bitmap = bitmap_buf.data();
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    if (!write_block(bitmap_block, bitmap_buf.get())) {
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

            KmBuf zbuf(4096);
            if (!zbuf || !zero_and_write_block(blk, zbuf.get())) { free_block(blk); return 0; }

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

            KmBuf zbuf(4096);
            if (!zbuf || !zero_and_write_block(indirect_blk, zbuf.get())) { free_block(indirect_blk); return 0; }

            disk.i_block[EXT2_INDIRECT_BLOCK] = indirect_blk;
        }

        uint32_t indirect_blk = disk.i_block[EXT2_INDIRECT_BLOCK];

        KmBuf buf(4096);
        if (!buf) {
            return 0;
        }
        if (!read_block(indirect_blk, buf.get())) {
            return 0;
        }

        auto* indirect = reinterpret_cast<uint32_t*>(buf.data());

        if (indirect[indirect_idx] == 0) {
            uint32_t data_blk = alloc_block();
            if (data_blk == 0) {
                return 0;
            }

            // zbuf is a separate buffer: zero_and_write_block zeroes zbuf (not
            // buf), so the indirect array in buf is still intact for patching.
            KmBuf zbuf(4096);
            if (!zbuf || !zero_and_write_block(data_blk, zbuf.get())) { free_block(data_blk); return 0; }

            indirect[indirect_idx] = data_blk;
            if (!write_block(indirect_blk, buf.get())) {
                return 0;
            }
        }

        return indirect[indirect_idx];
    }

    // Doubly-indirect block: i_block[13] points at a block of ptrs_per_block
    // single-indirect pointers, each of which points at a block of
    // ptrs_per_block data pointers.  Logical-index layout in this region:
    //   offset = file_block - (EXT2_DIRECT_BLOCKS + ptrs_per_block)
    //   idx1   = offset / ptrs_per_block   -> slot in the double-indirect block
    //   idx2   = offset % ptrs_per_block   -> slot in the chosen indirect block
    //
    // Two independent KmBufs (di_buf for the double-indirect array, child_buf
    // for each single-indirect child array): zero_and_write_block zeroes its
    // caller's buffer, so giving each freshly-allocated block its own zbuf
    // leaves the parent array intact -- no re-read needed after a child write
    // (the old shared-block_buf_ code had to re-read the parent each time
    // because zeroing the child clobbered the only buffer).
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

            KmBuf zbuf(4096);
            if (!zbuf || !zero_and_write_block(di_blk, zbuf.get())) { free_block(di_blk); return 0; }

            disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK] = di_blk;
        }
        const uint32_t di_blk = disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK];

        KmBuf di_buf(4096);
        if (!di_buf) {
            return 0;
        }
        // Level 1: the single-indirect child block at di_ptrs[idx1].
        if (!read_block(di_blk, di_buf.get())) {
            return 0;
        }
        auto*    di_ptrs   = reinterpret_cast<uint32_t*>(di_buf.data());
        uint32_t child_blk = di_ptrs[idx1];
        if (child_blk == 0) {
            child_blk = alloc_block();
            if (child_blk == 0) {
                return 0;
            }

            KmBuf zbuf(4096);
            if (!zbuf || !zero_and_write_block(child_blk, zbuf.get())) { free_block(child_blk); return 0; }
            // zbuf was a separate buffer, so di_ptrs (the double-indirect
            // array in di_buf) is still intact -- patch the slot and write.
            di_ptrs[idx1] = child_blk;
            if (!write_block(di_blk, di_buf.get())) {
                return 0;
            }
        }

        KmBuf child_buf(4096);
        if (!child_buf) {
            return 0;
        }
        // Level 2: the data block at child_ptrs[idx2].
        if (!read_block(child_blk, child_buf.get())) {
            return 0;
        }
        auto*    child_ptrs = reinterpret_cast<uint32_t*>(child_buf.data());
        uint32_t data_blk   = child_ptrs[idx2];
        if (data_blk == 0) {
            data_blk = alloc_block();
            if (data_blk == 0) {
                return 0;
            }

            KmBuf zbuf(4096);
            if (!zbuf || !zero_and_write_block(data_blk, zbuf.get())) { free_block(data_blk); return 0; }
            // zbuf was separate, so child_ptrs (the child array in child_buf)
            // is still intact -- patch and write.
            child_ptrs[idx2] = data_blk;
            if (!write_block(child_blk, child_buf.get())) {
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
