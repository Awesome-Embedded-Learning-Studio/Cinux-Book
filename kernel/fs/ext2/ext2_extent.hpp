/**
 * @file kernel/fs/ext2/ext2_extent.hpp
 * @brief ext4 extent tree (depth-0 leaf) logical→physical block resolver
 *
 * A pure helper that maps a logical file block to a physical disk block through
 * an ext4 extent tree stored in an inode's i_block[0..14].  Used by
 * Ext2FileOps::read to serve extent-mapped inodes (EXT4_EXTENTS_FL) on ext4
 * volumes; inodes without the flag keep using the classic indirect-block path.
 *
 * Only depth-0 leaf trees are handled.  Index nodes (depth > 0, used by very
 * large or fragmented files) report Unsupported so the caller stops the read
 * instead of silently misreading.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include "ext2_types.hpp"

namespace cinux::fs {

/// @brief Whether an on-disk inode is extent-mapped (EXT4_EXTENTS_FL set).
inline bool inode_has_extent_tree(const Ext2Inode& disk) {
    return (disk.i_flags & EXT4_EXTENTS_FL) != 0;
}

/// Outcome of resolving one logical block through an extent tree.
enum class ExtentLookupResult {
    Mapped,       ///< @p out_block holds the physical disk block to read
    Hole,         ///< Logical block is unmapped / uninitialized → caller zero-fills
    Unsupported,  ///< depth > 0 index tree or corrupt header → caller stops reading
};

/**
 * @brief Resolve a logical file block through the ext4 extent tree root
 *
 * Reads the 60-byte extent tree root directly from @p disk.i_block[0..14]
 * (no I/O).  depth-0 leaves only; see ExtentLookupResult for the outcomes.
 *
 * @param disk        On-disk inode whose i_block holds the extent tree root
 * @param file_block  Logical block index within the file
 * @param out_block   [out] Physical disk block when result is Mapped
 * @return Mapped / Hole / Unsupported
 */
ExtentLookupResult extent_lookup_block(const Ext2Inode& disk, uint32_t file_block,
                                       uint32_t& out_block);

/**
 * @brief Resolve one logical data block of an inode to its physical block (read)
 *
 * Read-side resolver shared by directory scans (lookup_in_dir, readdir) and any
 * small/direct-block reader.  Handles both ext4 extent trees and classic direct
 * pointers (i_block[0..11]); the regular-file read path keeps its own
 * single/double-indirect handling for large classic files.  Returns 0 for a
 * hole / unmapped / unsupported block — directory scans treat 0 as "skip".
 *
 * @param disk     On-disk inode
 * @param logical  Logical block index within the file
 * @return Physical disk block, or 0 if unmapped
 */
uint32_t inode_read_block(const Ext2Inode& disk, uint32_t logical);

}  // namespace cinux::fs
