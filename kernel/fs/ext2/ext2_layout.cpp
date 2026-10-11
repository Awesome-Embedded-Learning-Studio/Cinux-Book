/**
 * @file    ext2_layout.cpp
 * @brief   Layout pins and the pure block arithmetic of ext2.
 *
 * The structure sizes and field offsets are pinned here, next to the
 * arithmetic that shares them, so a layout mistake fails at build time
 * instead of corrupting a disk read at run time. The mode type segment
 * goes through the BitMask vocabulary like every other sub-byte field
 * in this tree.
 *
 * @author  Charliechen114514
 * @date    2026-10-10
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/ext2/ext2_layout.hpp"

#include <cstddef>
#include <cstdint>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/math.hpp"

namespace cinux::fs::ext2 {

static_assert(sizeof(Superblock) == 1024);
static_assert(sizeof(GroupDescriptor) == 32);
static_assert(sizeof(Inode) == 128);
static_assert(sizeof(DirEntry) == 8);

static_assert(offsetof(Superblock, s_magic) == 56);
static_assert(offsetof(Superblock, s_inode_size) == 88);
static_assert(offsetof(Superblock, s_log_block_size) == 24);
static_assert(offsetof(Superblock, s_inodes_per_group) == 40);
static_assert(offsetof(Superblock, s_feature_incompat) == 96);

static_assert(offsetof(GroupDescriptor, bg_inode_table) == 8);
static_assert(offsetof(GroupDescriptor, bg_free_blocks_count) == 12);

static_assert(offsetof(Inode, i_size) == 4);
static_assert(offsetof(Inode, i_links_count) == 26);
static_assert(offsetof(Inode, i_flags) == 32);
static_assert(offsetof(Inode, i_block) == 40);
static_assert(offsetof(Inode, i_mtime) == 16);

static_assert(kMagic == 0xEF53);
static_assert(kRootInode == 2);
static_assert(kSingleSlot == kDirectSlots);
static_assert(kDoubleSlot == kSingleSlot + 1);
static_assert(kTripleSlot == kDoubleSlot + 1);
static_assert(kDirEntryHeadBytes == sizeof(DirEntry));
static_assert(kMaxGroups * sizeof(GroupDescriptor) <= kVolumeBlockBytes);

namespace {

/// One indirect table holds one block of four-byte pointers.
constexpr unsigned long kSingleTableSlots = kVolumeBlockBytes / sizeof(std::uint32_t);

constexpr unsigned long kDoubleTableSlots = kSingleTableSlots * kSingleTableSlots;

/// The first file block the single-indirect layer serves.
constexpr unsigned long kSingleFirst = kDirectSlots;

/// The first file block the double-indirect layer serves.
constexpr unsigned long kDoubleFirst = kDirectSlots + kSingleTableSlots;

/// The mode word's type segment: the top four bits.
constexpr base::bit::BitRange kModeTypeSegment{.low = 12, .width = 4};

/// The type-segment value saying directory.
constexpr std::uint16_t kModeTypeDirectory = 0x4;

/// The type-segment value saying regular file.
constexpr std::uint16_t kModeTypeRegular = 0x8;

}  // namespace

unsigned long DecodeBlockSize(std::uint32_t log_block_size) {
    return kVolumeBlockBytes << log_block_size;
}

unsigned long GroupOf(std::uint32_t inode, std::uint32_t inodes_per_group) {
    return (inode - 1) / inodes_per_group;
}

unsigned long IndexOfInode(std::uint32_t inode, std::uint32_t inodes_per_group) {
    return (inode - 1) % inodes_per_group;
}

unsigned long DirEntryLength(unsigned long name_len) {
    return base::math::AlignUp(kDirEntryHeadBytes + name_len, kDirEntryAlign);
}

BlockSlot MapFileBlock(unsigned long file_block) {
    if (file_block < kSingleFirst) {
        return {.kind = BlockSlot::Kind::kDirect, .slot = file_block, .outer = 0, .inner = 0};
    }
    if (file_block < kDoubleFirst) {
        return {.kind  = BlockSlot::Kind::kSingle,
                .slot  = kSingleSlot,
                .outer = file_block - kSingleFirst,
                .inner = 0};
    }
    if (file_block < kDoubleFirst + kDoubleTableSlots) {
        const unsigned long kPast = file_block - kDoubleFirst;
        return {.kind  = BlockSlot::Kind::kDouble,
                .slot  = kDoubleSlot,
                .outer = kPast / kSingleTableSlots,
                .inner = kPast % kSingleTableSlots};
    }
    return {.kind = BlockSlot::Kind::kUnsupported, .slot = 0, .outer = 0, .inner = 0};
}

bool IsDirectoryMode(std::uint16_t mode) {
    const base::bit::BitMask<std::uint16_t> kWord{mode};
    return kWord.extract(kModeTypeSegment) == kModeTypeDirectory;
}

bool IsRegularMode(std::uint16_t mode) {
    const base::bit::BitMask<std::uint16_t> kWord{mode};
    return kWord.extract(kModeTypeSegment) == kModeTypeRegular;
}

}  // namespace cinux::fs::ext2
