/**
 * @file kernel/fs/ext2/ext2_extent.cpp
 * @brief ext4 extent tree (depth-0 leaf) block resolver
 *
 * Parses the 60-byte extent tree root stored in an inode's i_block[0..14] and
 * maps a logical file block to its physical disk block.  Handles depth-0 leaf
 * extents only; depth > 0 index nodes are reported Unsupported (follow-up).
 */

#include "ext2_extent.hpp"

#include <stdint.h>

#include "ext2_types.hpp"

namespace cinux::fs {

ExtentLookupResult extent_lookup_block(const Ext2Inode& disk, uint32_t file_block,
                                       uint32_t& out_block) {
    // The extent tree root occupies the full 60-byte i_block[0..14] region.
    auto* tree = reinterpret_cast<const uint8_t*>(disk.i_block);
    auto* hdr  = reinterpret_cast<const Ext4ExtentHeader*>(tree);

    if (hdr->eh_magic != EXT4_EXTENT_MAGIC) {
        // Flagged extent-based but the header is absent/corrupt: do not guess.
        return ExtentLookupResult::Unsupported;
    }
    if (hdr->eh_depth != 0) {
        // Index nodes (depth > 0) need a follow-up reader; bail honestly.
        return ExtentLookupResult::Unsupported;
    }

    auto*    extents = reinterpret_cast<const Ext4Extent*>(tree + sizeof(Ext4ExtentHeader));
    uint16_t count   = hdr->eh_entries;

    for (uint16_t i = 0; i < count; ++i) {
        const Ext4Extent& e      = extents[i];
        uint32_t          log    = e.ee_block;
        uint16_t          raw    = e.ee_len;
        bool              uninit = raw > EXT4_EXTENT_INIT_LEN_MAX;
        // Uninitialized extents encode real length as ee_len - 32768.
        uint32_t len = uninit ? static_cast<uint32_t>(raw - EXT4_EXTENT_INIT_LEN_MAX) : raw;

        if (file_block >= log && file_block < log + len) {
            if (uninit) {
                // Preallocated-but-unwritten region: reads return zeros.
                return ExtentLookupResult::Hole;
            }
            uint64_t phys_start = (static_cast<uint64_t>(e.ee_start_hi) << 32) | e.ee_start_lo;
            out_block           = static_cast<uint32_t>(phys_start + (file_block - log));
            return ExtentLookupResult::Mapped;
        }
    }

    // No extent covers this logical block: a hole (sparse file) → zero-fill.
    return ExtentLookupResult::Hole;
}

uint32_t inode_read_block(const Ext2Inode& disk, uint32_t logical) {
    if (inode_has_extent_tree(disk)) {
        uint32_t           phys = 0;
        ExtentLookupResult r    = extent_lookup_block(disk, logical, phys);
        return (r == ExtentLookupResult::Mapped) ? phys : 0;
    }
    if (logical < EXT2_DIRECT_BLOCKS) {
        return disk.i_block[logical];
    }
    // Beyond the direct-pointer region and not extent-mapped: directory scans cap
    // at EXT2_DIRECT_BLOCKS anyway, so this is unreachable for them.
    return 0;
}

}  // namespace cinux::fs
