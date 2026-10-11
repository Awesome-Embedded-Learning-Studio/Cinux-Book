/**
 * @file    ext2_layout.hpp
 * @brief   The on-disk words of ext2: layouts, offsets, block arithmetic.
 *
 * Everything here describes the bytes as they sit on the disk — the
 * superblock at byte 1024, the group descriptors after the first data
 * block, the 128-byte inodes inside their tables, the variable-length
 * directory entries walking by rec_len. The chip is little-endian and so
 * is this machine, so the structures are read in place through the
 * direct map, no byte swapping anywhere. The block-mapping arithmetic —
 * which i_block slot serves a given file block, and which indexes walk
 * the indirect trees — lives here as pure functions, shared by the read
 * and the later write path so the two can never drift apart. Constants
 * with more than one consumer (the magic, the root inode, the slot
 * numbers) are declared here and pinned by static_assert in the .cpp;
 * the rest is private to the implementation.
 *
 * @author  Charliechen114514
 * @date    2026-10-10
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>

#include "kernel/driver/block/block_config.hpp"

namespace cinux::fs::ext2 {

/// The format's signature, at superblock offset 56.
inline constexpr std::uint16_t kMagic = 0xEF53;

/// The inode number of the root directory.
inline constexpr std::uint32_t kRootInode = 2;

/// How many i_block slots hold direct block numbers.
inline constexpr unsigned long kDirectSlots = 12;

/// The i_block slot holding the single-indirect pointer.
inline constexpr unsigned long kSingleSlot = 12;

/// The i_block slot holding the double-indirect pointer.
inline constexpr unsigned long kDoubleSlot = 13;

/// The i_block slot holding the triple-indirect pointer (unused here).
inline constexpr unsigned long kTripleSlot = 14;

/// The format's base block size, the smallest any volume may carry.
inline constexpr unsigned long kVolumeBlockBytes = 1024;

/// Sectors one volume block spans; every block-to-Lba spelling goes
/// through this one word.
inline constexpr unsigned long kBlockSectors = kVolumeBlockBytes / driver::kSectorBytes;

/// 32-bit words one volume block holds; the indirect-table scratch
/// buffers size themselves with this.
inline constexpr unsigned long kBlockWords = kVolumeBlockBytes / sizeof(std::uint32_t);

/// The sector the superblock starts at: byte 1024 of the volume.
inline constexpr unsigned long kSuperblockSector = kBlockSectors;

/// Bytes in one directory-entry head before the name bytes begin.
inline constexpr unsigned long kDirEntryHeadBytes = 8;

/// rec_len values are multiples of this many bytes.
inline constexpr unsigned long kDirEntryAlign = 4;

/// How many block groups a mounted volume may carry; beyond this is refused.
inline constexpr unsigned long kMaxGroups = 32;

/**
 * @brief   The superblock: one copy past byte 1024 of the volume.
 * @note    Fields follow the ext2 specification order; the tail padding
 *          carries the journal and algorithm blocks this kernel never
 *          reads. Only the fields with a consumer are named.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct Superblock {
    std::uint32_t s_inodes_count;        ///< Inodes on the volume.
    std::uint32_t s_blocks_count;        ///< Blocks on the volume.
    std::uint32_t s_r_blocks_count;      ///< Blocks reserved for root.
    std::uint32_t s_free_blocks_count;   ///< Free blocks, volume-wide.
    std::uint32_t s_free_inodes_count;   ///< Free inodes, volume-wide.
    std::uint32_t s_first_data_block;    ///< Block 1 when blocks are 1 KiB, else 0.
    std::uint32_t s_log_block_size;      ///< Block size is 1024 shifted left by this.
    std::uint32_t s_log_frag_size;       ///< Fragment size log, unused here.
    std::uint32_t s_blocks_per_group;    ///< Data blocks each group covers.
    std::uint32_t s_frags_per_group;     ///< Fragments per group, unused here.
    std::uint32_t s_inodes_per_group;    ///< Inodes each group's table holds.
    std::uint32_t s_mtime;               ///< Last mount time, seconds.
    std::uint32_t s_wtime;               ///< Last write time, seconds.
    std::uint16_t s_mnt_count;           ///< Mounts since the last check.
    std::uint16_t s_max_mnt_count;       ///< Mounts allowed before a check.
    std::uint16_t s_magic;               ///< Must read 0xEF53.
    std::uint16_t s_state;               ///< Clean or errored.
    std::uint16_t s_errors;              ///< What to do on an error.
    std::uint16_t s_minor_rev_level;     ///< Minor revision number.
    std::uint32_t s_lastcheck;           ///< Time of the last check.
    std::uint32_t s_checkinterval;       ///< Seconds between checks.
    std::uint32_t s_creator_os;          ///< Which OS made this volume.
    std::uint32_t s_rev_level;           ///< 0 good-old, 1 dynamic; this backend mounts rev 1 only.
    std::uint16_t s_def_resuid;          ///< Reserved-block user id.
    std::uint16_t s_def_resgid;          ///< Reserved-block group id.
    std::uint32_t s_first_ino;           ///< First non-reserved inode, rev 1.
    std::uint16_t s_inode_size;          ///< Bytes per inode, rev 1.
    std::uint16_t s_block_group_nr;      ///< Which group this copy serves.
    std::uint32_t s_feature_compat;      ///< Compatible feature bits.
    std::uint32_t s_feature_incompat;    ///< Must-understand feature bits.
    std::uint32_t s_feature_ro_compat;   ///< Mount-read-only feature bits.
    std::uint8_t  s_uuid[16];            ///< Volume identifier.
    std::uint8_t  s_volume_name[16];     ///< NUL-padded volume name.
    std::uint8_t  s_last_mounted[64];    ///< Last mount point path.
    std::uint8_t  s_algorithm_area[64];  ///< Rev-1 algorithm, prealloc and
                                         ///< journal fields this backend
                                         ///< never reads.
    std::uint32_t s_mkfs_time;           ///< Time the volume was made.
    std::uint8_t  s_tail[756];           ///< Padding to 1024 bytes.
};

/**
 * @brief   One block group's descriptor, 32 bytes each in the table.
 * @note    The three leading block numbers are the group's whole
 *          geography: its two bitmaps and its inode table.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct GroupDescriptor {
    std::uint32_t bg_block_bitmap;       ///< First block of the block bitmap.
    std::uint32_t bg_inode_bitmap;       ///< First block of the inode bitmap.
    std::uint32_t bg_inode_table;        ///< First block of the inode table.
    std::uint16_t bg_free_blocks_count;  ///< Free blocks in this group.
    std::uint16_t bg_free_inodes_count;  ///< Free inodes in this group.
    std::uint16_t bg_used_dirs_count;    ///< Directories in this group.
    std::uint16_t bg_pad;                ///< Alignment padding.
    std::uint8_t  bg_reserved[12];       ///< Unused padding to 32 bytes.
};

/**
 * @brief   One inode, 128 bytes in its group's table.
 * @note    i_block's fifteen slots are the file's data map: twelve
 *          direct numbers, then single, double and triple indirect
 *          pointers. i_blocks counts in 512-byte sectors whatever the
 *          volume's block size is.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct Inode {
    std::uint16_t i_mode;         ///< Type and permission bits.
    std::uint16_t i_uid;          ///< Owner's user id.
    std::uint32_t i_size;         ///< File bytes, low half.
    std::uint32_t i_atime;        ///< Access time, seconds.
    std::uint32_t i_ctime;        ///< Inode change time, seconds.
    std::uint32_t i_mtime;        ///< Content change time, seconds.
    std::uint32_t i_dtime;        ///< Deletion time, seconds.
    std::uint16_t i_gid;          ///< Owner's group id.
    std::uint16_t i_links_count;  ///< Hard links pointing here.
    std::uint32_t i_blocks;       ///< Data in 512-byte sector units.
    std::uint32_t i_flags;        ///< Attribute flags: extents, immutable, ...
    std::uint32_t i_osd1;         ///< OS-specific word, unused.
    std::uint32_t i_block[15];    ///< Direct then indirect pointers.
    std::uint32_t i_generation;   ///< File version number.
    std::uint32_t i_file_acl;     ///< Extended-attribute block, unused.
    std::uint32_t i_dir_acl;      ///< Size high half under HugeFile, else 0.
    std::uint32_t i_faddr;        ///< Fragment address, unused.
    std::uint8_t  i_osd2[12];     ///< OS-specific bytes, unused.
};

/**
 * @brief   The directory-entry head; the name bytes follow immediately.
 * @note    Variable length: entries advance by rec_len, which is always
 *          4-aligned and covers the head plus the name. name_len is a
 *          16-bit field in the no-filetype layout this volume uses; a
 *          name never reaches 256 bytes, so the high byte stays zero
 *          either way. The entry's kind is read from the target inode's
 *          mode, never from a type byte.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct DirEntry {
    std::uint32_t inode;     ///< The entry's inode number, 0 when unused.
    std::uint16_t rec_len;   ///< Bytes to the next entry, 4-aligned.
    std::uint16_t name_len;  ///< Name bytes that follow the head.
};

/**
 * @brief   How one file block is served: which layer holds its pointer.
 * @note    The read and the write path ask this same question, so the
 *          answer lives here once. Beyond double lies triple, which
 *          this kernel deliberately does not walk.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct BlockSlot {
    /// Which pointer layer serves the block.
    enum class Kind : std::uint8_t {
        kDirect,       ///< i_block[file_block] is the block number.
        kSingle,       ///< i_block[12] leads one table of pointers.
        kDouble,       ///< i_block[13] leads a table of tables.
        kUnsupported,  ///< Past double: triple, not walked.
    };

    Kind          kind;   ///< Which layer was chosen.
    unsigned long slot;   ///< The i_block index to read.
    unsigned long outer;  ///< The first indirect table's index.
    unsigned long inner;  ///< The second indirect table's index.
};

/**
 * @brief         Decodes the block size field.
 *
 * @param[in]     log_block_size   The superblock's shift value.
 * @return        1024 shifted left by that value, in bytes.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] unsigned long DecodeBlockSize(std::uint32_t log_block_size);

/**
 * @brief         Which group an inode number lives in.
 *
 * @param[in]     inode              The inode number, one-based.
 * @param[in]     inodes_per_group   The superblock's group size.
 * @return        The group index, zero-based.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] unsigned long GroupOf(std::uint32_t inode, std::uint32_t inodes_per_group);

/**
 * @brief         Where an inode sits inside its group's table.
 *
 * @param[in]     inode              The inode number, one-based.
 * @param[in]     inodes_per_group   The superblock's group size.
 * @return        The inode's zero-based index within the group.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] unsigned long IndexOfInode(std::uint32_t inode, std::uint32_t inodes_per_group);

/**
 * @brief         The 4-aligned record length a name needs.
 *
 * @param[in]     name_len   Name bytes, without the head.
 * @return        (8 + name_len + 3) rounded down to a multiple of 4.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] unsigned long DirEntryLength(unsigned long name_len);

/**
 * @brief         Maps a file block onto its pointer layer.
 *
 * @param[in]     file_block   The block index within the file.
 * @return        Which i_block slot serves it and the indexes to walk.
 * @note          File blocks past the double layer come back
 *               kUnsupported; the caller refuses them loudly.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] BlockSlot MapFileBlock(unsigned long file_block);

/**
 * @brief         Reads the type segment out of an inode mode.
 *
 * @param[in]     mode   The raw i_mode value.
 * @return        True when the type segment says directory.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] bool IsDirectoryMode(std::uint16_t mode);

/**
 * @brief         Reads the type segment out of an inode mode.
 *
 * @param[in]     mode   The raw i_mode value.
 * @return        True when the type segment says regular file.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] bool IsRegularMode(std::uint16_t mode);

}  // namespace cinux::fs::ext2
