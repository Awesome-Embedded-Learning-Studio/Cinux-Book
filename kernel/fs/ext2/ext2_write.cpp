/**
 * @file    ext2_write.cpp
 * @brief   The write faces: allocators, directory entries, file
 *          growth — every change a metadata breath under the mutex.
 *
 * A write is a sequence of read-modify-write steps over the cache:
 * copy the bitmap block to stack words, find a clear bit, set it,
 * patch the changed byte through, discount the counters in the
 * superblock and the group descriptor. The mutex spans the whole
 * sequence because interleaved sequences would let two files claim
 * the same block — the one race this backend refuses to host. The
 * file map grows lazily: a hole asks for its block on first write,
 * through the same three-layer walk the read path uses, so the two
 * can never disagree about where a block lives.
 *
 * @author  Charliechen114514
 * @date    2026-10-10
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cinux/bit_ops/bitmap.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/path.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/ext2/ext2_layout.hpp"
#include "kernel/fs/ext2/ext2_mutation.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs::ext2 {

namespace {

/// Bitmap bits one bitmap block covers.
constexpr unsigned long kBitsPerBitmapBlock = kVolumeBlockBytes * 8;

/// Bitmap words one block holds, for the stack-side RMW copy.
constexpr unsigned long kBitmapWords = kVolumeBlockBytes / sizeof(base::bit::WordMask);

const std::uint8_t* byte_of(base::bit::WordMask* words, unsigned long byte_index) {
    auto* const kRaw = reinterpret_cast<std::uint8_t*>(words);
    return kRaw + byte_index;
}

void entry_bytes(std::uint8_t* out, unsigned long ino, std::string_view name) {
    base::WriteWord32(out, 0, static_cast<std::uint32_t>(ino));
    base::WriteWord16(out, 4, static_cast<std::uint16_t>(DirEntryLength(name.size())));
    out[6] = static_cast<std::uint8_t>(name.size());
    out[7] = 0;
    base::CopyBytes(out + kDirEntryHeadBytes, name.data(), name.size());
}

[[nodiscard]] bool is_dots(const DirEntry& entry) {
    if (entry.name_len == 1) {
        const char* const kName = reinterpret_cast<const char*>(&entry) + kDirEntryHeadBytes;
        return kName[0] == '.';
    }
    if (entry.name_len == 2) {
        const char* const kName = reinterpret_cast<const char*>(&entry) + kDirEntryHeadBytes;
        return kName[0] == '.' && kName[1] == '.';
    }
    return false;
}

}  // namespace

base::Result<FsNode> Ext2::make_file(std::string_view path) {
    const proc::MutexGuard kGate(write_gate_);
    return make_file_locked(path);
}
base::Result<FsNode> Ext2::make_file_held(std::string_view path) {
    const proc::MutexGuard kGate(write_gate_);
    const auto             kNode = make_file_locked(path);
    if (kNode.ok()) {
        retain_locked(kNode.value());
    }
    return kNode;
}
base::Result<FsNode> Ext2::make_file_locked(std::string_view path) {
    if (!mounted_) {
        return base::KernelError::kIOError;
    }

    std::string_view parent;
    std::string_view leaf;
    const bool       kHadSlash = base::SplitLeaf(path, parent, leaf);
    if (leaf.empty() || leaf.size() >= kFsNameMax) {
        return base::KernelError::kInvalidArgument;
    }
    const base::Result<FsNode> kParentNode = lookup_locked(parent);
    if (!kParentNode.ok()) {
        return kParentNode.error();
    }
    static_cast<void>(kHadSlash);
    return seat_new_file(kParentNode.value().ino, leaf);
}

base::Result<FsNode> Ext2::make_file_at(FsNode base, std::string_view name) {
    const proc::MutexGuard kGate(write_gate_);
    if (!mounted_ || base.type != InodeType::kDirectory || name.empty() ||
        name.size() >= kFsNameMax || name.contains('/')) {
        return base::KernelError::kInvalidArgument;
    }
    const auto kNode = seat_new_file(base.ino, name);
    if (kNode.ok()) {
        retain_locked(kNode.value());
    }
    return kNode;
}

base::Result<FsNode> Ext2::seat_new_file(unsigned long parent_ino, std::string_view leaf) {
    const base::Result<Inode> kParentRead = read_inode(parent_ino);
    if (!kParentRead.ok()) {
        return kParentRead.error();
    }
    if (!IsDirectoryMode(kParentRead.value().i_mode)) {
        return base::KernelError::kNotADirectory;
    }
    if (kParentRead.value().i_links_count == 0) {
        return base::KernelError::kNotFound;
    }
    Inode parent_inode = kParentRead.value();

    const base::Result<unsigned long> kExisting = lookup_in_dir(parent_inode, leaf);
    if (kExisting.ok()) {
        return base::KernelError::kAlreadyExists;
    }
    if (kExisting.error() != base::KernelError::kNotFound) {
        return kExisting.error();
    }

    const base::Result<unsigned long> kIno = alloc_inode(false);
    if (!kIno.ok()) {
        return kIno.error();
    }

    const std::uint32_t kStamp = now_stamp();
    const Inode         kFresh{
        .i_mode        = static_cast<std::uint16_t>(kFsFileMode),
        .i_uid         = 0,
        .i_size        = 0,
        .i_atime       = kStamp,
        .i_ctime       = kStamp,
        .i_mtime       = kStamp,
        .i_dtime       = 0,
        .i_gid         = 0,
        .i_links_count = 1,
        .i_blocks      = 0,
        .i_flags       = 0,
        .i_osd1        = 0,
        .i_block       = {},
        .i_generation  = 0,
        .i_file_acl    = 0,
        .i_dir_acl     = 0,
        .i_faddr       = 0,
        .i_osd2        = {},
    };
    if (!write_inode(kIno.value(), kFresh)) {
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }
    if (!add_dir_entry(parent_inode, parent_ino, kIno.value(), leaf)) {
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }
    return FsNode{.opaque = nullptr, .type = InodeType::kFile, .ino = kIno.value(), .size = 0};
}

base::Result<FsNode> Ext2::make_dir(std::string_view path) {
    const proc::MutexGuard kGate(write_gate_);
    if (!mounted_) {
        return base::KernelError::kIOError;
    }

    std::string_view parent;
    std::string_view leaf;
    static_cast<void>(base::SplitLeaf(path, parent, leaf));
    if (leaf.empty() || leaf.size() >= kFsNameMax) {
        return base::KernelError::kInvalidArgument;
    }
    const base::Result<FsNode> kParentNode = lookup_locked(parent);
    if (!kParentNode.ok()) {
        return kParentNode.error();
    }
    const base::Result<Inode> kParentRead = read_inode(kParentNode.value().ino);
    if (!kParentRead.ok()) {
        return kParentRead.error();
    }
    if (!IsDirectoryMode(kParentRead.value().i_mode)) {
        return base::KernelError::kNotADirectory;
    }
    Inode                             parent_inode = kParentRead.value();
    const base::Result<unsigned long> kExisting    = lookup_in_dir(parent_inode, leaf);
    if (kExisting.ok()) {
        return base::KernelError::kAlreadyExists;
    }
    if (kExisting.error() != base::KernelError::kNotFound) {
        return kExisting.error();
    }

    const base::Result<unsigned long> kIno = alloc_inode(true);
    if (!kIno.ok()) {
        return kIno.error();
    }
    const base::Result<unsigned long> kBlock = alloc_block();
    if (!kBlock.ok()) {
        free_inode(kIno.value());
        return kBlock.error();
    }

    Inode fresh{};
    fresh.i_mode        = static_cast<std::uint16_t>(kFsDirMode);
    fresh.i_size        = kVolumeBlockBytes;
    fresh.i_links_count = 2;
    fresh.i_atime       = now_stamp();
    fresh.i_ctime       = fresh.i_atime;
    fresh.i_mtime       = fresh.i_atime;
    fresh.i_block[0]    = kBlock.value();
    fresh.i_blocks      = kBlockSectors;
    if (!write_inode(kIno.value(), fresh)) {
        free_block(kBlock.value());
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }

    const std::uint8_t kZeroes[kVolumeBlockBytes] = {};
    if (!patch_storage(driver::Lba{.value = kBlock.value() * kBlockSectors}, 0, kVolumeBlockBytes,
                       kZeroes)) {
        free_block(kBlock.value());
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }

    std::uint8_t dot[12]    = {};
    std::uint8_t dotdot[12] = {};
    entry_bytes(dot, kIno.value(), ".");
    entry_bytes(dotdot, kParentNode.value().ino, "..");
    const unsigned long kDotRec  = DirEntryLength(1);
    const unsigned long kDotsRec = kVolumeBlockBytes - kDotRec;
    base::WriteWord16(dotdot, 4, static_cast<std::uint16_t>(kDotsRec));

    const driver::Lba kBlockLba{.value = kBlock.value() * kBlockSectors};
    if (!patch_storage(kBlockLba, 0, kDotRec, dot) ||
        !patch_storage(kBlockLba, kDotRec, sizeof(dotdot), dotdot) ||
        !add_dir_entry(parent_inode, kParentNode.value().ino, kIno.value(), leaf)) {
        free_block(kBlock.value());
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }

    Inode linked         = parent_inode;
    linked.i_links_count = static_cast<std::uint16_t>(linked.i_links_count + 1U);
    if (!write_inode(kParentNode.value().ino, linked)) {
        free_block(kBlock.value());
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }
    return FsNode{.opaque = nullptr, .type = InodeType::kDirectory, .ino = kIno.value(), .size = 0};
}

base::Result<void> Ext2::unlink(std::string_view path) {
    const proc::MutexGuard kGate(write_gate_);
    Mutation               mutation(*this);
    const auto             kResult = unlink_locked(path);
    if (!kResult.ok()) {
        return kResult;
    }
    return mutation.commit() ? base::Result<void>{} : base::KernelError::kIOError;
}

base::Result<void> Ext2::retire_empty_directory(unsigned long ino, const Inode& dead_dir,
                                                unsigned long parent_ino, const Inode& parent) {
    Inode dead         = dead_dir;
    dead.i_links_count = static_cast<std::uint16_t>(dead.i_links_count - 2U);
    if (dead.i_links_count == 0 && !has_hold(ino)) {
        dead.i_dtime = clock_ == nullptr ? sb_.s_mkfs_time : now_stamp();
        free_data_blocks(dead);
    }
    if (!write_inode(ino, dead)) {
        return base::KernelError::kIOError;
    }
    if (dead.i_links_count == 0 && !has_hold(ino)) {
        free_inode(ino);
    }
    Inode linked         = parent;
    linked.i_links_count = static_cast<std::uint16_t>(linked.i_links_count - 1U);
    if (!write_inode(parent_ino, linked)) {
        return base::KernelError::kIOError;
    }
    const unsigned long kGroup = GroupOf(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    static_cast<void>(adjust_used_dirs(kGroup, -1));
    return {};
}

base::Result<bool> Ext2::directory_is_empty(const Inode& victim) {
    bool                     has_child = false;
    const base::Result<bool> kWalk     = walk_entries(victim, [&](const DirEntry& entry) {
        if (entry.inode != 0 && !is_dots(entry)) {
            has_child = true;
            return false;
        }
        return true;
    });
    if (!kWalk.ok()) {
        return kWalk.error();
    }
    return !has_child;
}

base::Result<void> Ext2::unlink_locked(std::string_view path) {
    if (!mounted_) {
        return base::KernelError::kIOError;
    }

    std::string_view parent;
    std::string_view leaf;
    static_cast<void>(base::SplitLeaf(path, parent, leaf));
    if (leaf.empty()) {
        return base::KernelError::kInvalidArgument;
    }
    const base::Result<FsNode> kParentNode = lookup_locked(parent);
    if (!kParentNode.ok()) {
        return kParentNode.error();
    }
    const base::Result<Inode> kParentRead = read_inode(kParentNode.value().ino);
    if (!kParentRead.ok()) {
        return kParentRead.error();
    }
    const Inode&                      parent_inode = kParentRead.value();
    const base::Result<unsigned long> kIno         = lookup_in_dir(parent_inode, leaf);
    if (!kIno.ok()) {
        return kIno.error();
    }
    const base::Result<Inode> kVictimRead = read_inode(kIno.value());
    if (!kVictimRead.ok()) {
        return kVictimRead.error();
    }
    const Inode& victim_inode = kVictimRead.value();

    if (IsDirectoryMode(victim_inode.i_mode)) {
        const base::Result<bool> kEmpty = directory_is_empty(victim_inode);
        if (!kEmpty.ok()) {
            return kEmpty.error();
        }
        if (!kEmpty.value()) {
            return base::KernelError::kDirectoryNotEmpty;
        }
    }

    if (!remove_dir_entry(parent_inode, leaf)) {
        return base::KernelError::kIOError;
    }

    if (IsDirectoryMode(victim_inode.i_mode)) {
        const auto kRetired = retire_empty_directory(kIno.value(), kVictimRead.value(),
                                                     kParentNode.value().ino, parent_inode);
        if (!kRetired.ok()) {
            return kRetired.error();
        }
    } else {
        const auto kDropped = drop_file_link(kIno.value());
        if (!kDropped.ok()) {
            return kDropped.error();
        }
    }
    return {};
}

base::Result<Ext2::RenameEnds> Ext2::resolve_rename_ends(std::string_view old_parent,
                                                         std::string_view new_parent) {
    const auto kOldParent = lookup_locked(old_parent);
    const auto kNewParent = lookup_locked(new_parent);
    if (!kOldParent.ok()) {
        return kOldParent.error();
    }
    if (!kNewParent.ok()) {
        return kNewParent.error();
    }
    if (kOldParent.value().type != InodeType::kDirectory ||
        kNewParent.value().type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }
    const auto kOldInode = read_inode(kOldParent.value().ino);
    const auto kNewInode = read_inode(kNewParent.value().ino);
    if (!kOldInode.ok() || !kNewInode.ok()) {
        return base::KernelError::kIOError;
    }
    return RenameEnds{
        .old_parent = kOldParent.value(),
        .new_parent = kNewParent.value(),
        .old_dir    = kOldInode.value(),
        .new_dir    = kNewInode.value(),
    };
}

bool Ext2::seat_new_name(RenameEnds& ends, std::string_view new_leaf, unsigned long source_ino,
                         bool replacing) {
    if (replacing) {
        return replace_dir_entry_ino(ends.new_dir, new_leaf, source_ino);
    }
    return add_dir_entry(ends.new_dir, ends.new_parent.ino, source_ino, new_leaf);
}

base::Result<void> Ext2::swap_names(RenameEnds& ends, const RenamePlan& plan) {
    if (!seat_new_name(ends, plan.new_leaf, plan.source_ino, plan.replacing)) {
        return base::KernelError::kIOError;
    }
    const auto kCurrentOld = read_inode(ends.old_parent.ino);
    if (!kCurrentOld.ok() || !remove_dir_entry(kCurrentOld.value(), plan.old_leaf)) {
        return base::KernelError::kIOError;
    }
    return {};
}

base::Result<void> Ext2::rename(std::string_view source, std::string_view target) {
    const proc::MutexGuard kGate(write_gate_);
    Mutation               mutation(*this);
    std::string_view       old_parent;
    std::string_view       old_leaf;
    std::string_view       new_parent;
    std::string_view       new_leaf;
    static_cast<void>(base::SplitLeaf(source, old_parent, old_leaf));
    static_cast<void>(base::SplitLeaf(target, new_parent, new_leaf));
    if (old_leaf.empty() || new_leaf.empty() || new_leaf == "." || new_leaf == "..") {
        return base::KernelError::kInvalidArgument;
    }
    auto ends = resolve_rename_ends(old_parent, new_parent);
    if (!ends.ok()) {
        return ends.error();
    }
    const auto kSource = lookup_in_dir(ends.value().old_dir, old_leaf);
    if (!kSource.ok()) {
        return kSource.error();
    }
    const auto kSourceInode = read_inode(kSource.value());
    if (!kSourceInode.ok()) {
        return kSourceInode.error();
    }
    if (!IsRegularMode(kSourceInode.value().i_mode)) {
        return base::KernelError::kNotSupported;
    }
    const auto kTarget   = lookup_in_dir(ends.value().new_dir, new_leaf);
    bool       replacing = false;
    if (kTarget.ok()) {
        if (kTarget.value() == kSource.value()) {
            return {};
        }
        const auto kTargetInode = read_inode(kTarget.value());
        if (!kTargetInode.ok()) {
            return kTargetInode.error();
        }
        if (IsDirectoryMode(kTargetInode.value().i_mode)) {
            return base::KernelError::kIsADirectory;
        }
        replacing = true;
    } else if (kTarget.error() != base::KernelError::kNotFound) {
        return kTarget.error();
    }
    const RenamePlan kPlan{
        .old_leaf   = old_leaf,
        .new_leaf   = new_leaf,
        .source_ino = kSource.value(),
        .target_ino = replacing ? kTarget.value() : 0U,
        .replacing  = replacing,
    };
    const auto kSwapped = swap_names(ends.value(), kPlan);
    if (!kSwapped.ok()) {
        return kSwapped.error();
    }
    if (kPlan.replacing) {
        const auto kDropped = drop_file_link(kPlan.target_ino);
        if (!kDropped.ok()) {
            return kDropped.error();
        }
    }
    return mutation.commit() ? base::Result<void>{} : base::KernelError::kIOError;
}

base::Result<void> Ext2::chmod(FsNode node, unsigned int mode) {
    const proc::MutexGuard kGate(write_gate_);
    auto                   inode = read_inode(node.ino);
    if (!inode.ok()) {
        return inode.error();
    }
    base::bit::BitMask<std::uint16_t> mode_word{inode.value().i_mode};
    mode_word.deposit(kModePermissionSegment, static_cast<std::uint16_t>(mode));
    inode.value().i_mode  = mode_word.raw;
    inode.value().i_ctime = now_stamp();
    return write_inode(node.ino, inode.value()) ? base::Result<void>{}
                                                : base::KernelError::kIOError;
}

base::Result<unsigned long> Ext2::write(FsNode node, unsigned long offset, const void* source,
                                        unsigned long count) {
    const proc::MutexGuard kGate(write_gate_);
    return write_locked(node, offset, source, count);
}

base::Result<unsigned long> Ext2::append(FsNode node, unsigned long& offset, const void* source,
                                         unsigned long count) {
    const proc::MutexGuard kGate(write_gate_);
    const auto             kInode = read_inode(node.ino);
    if (!kInode.ok()) {
        return kInode.error();
    }
    const unsigned long kEnd = kInode.value().i_size;
    const auto          kPut = write_locked(node, kEnd, source, count);
    if (kPut.ok()) {
        offset = kEnd + kPut.value();
    }
    return kPut;
}

base::Result<void> Ext2::truncate(FsNode node) {
    const proc::MutexGuard kGate(write_gate_);
    if (!mounted_) {
        return base::KernelError::kIOError;
    }
    if (node.type != InodeType::kFile) {
        return base::KernelError::kIsADirectory;
    }
    const auto kRead = read_inode(node.ino);
    if (!kRead.ok()) {
        return kRead.error();
    }
    Inode emptied = kRead.value();
    free_data_blocks(emptied);
    base::SetBytes(emptied.i_block, 0, sizeof(emptied.i_block));
    emptied.i_size   = 0;
    emptied.i_blocks = 0;
    emptied.i_mtime  = now_stamp();
    emptied.i_ctime  = emptied.i_mtime;
    if (!write_inode(node.ino, emptied)) {
        return base::KernelError::kIOError;
    }
    return {};
}

base::Result<unsigned long> Ext2::write_locked(FsNode node, unsigned long offset,
                                               const void* source, unsigned long count) {
    if (!mounted_) {
        return base::KernelError::kIOError;
    }
    if (node.type == InodeType::kDirectory) {
        return base::KernelError::kIsADirectory;
    }
    if (count == 0) {
        return 0UL;
    }

    const base::Result<Inode> kFileRead = read_inode(node.ino);
    if (!kFileRead.ok()) {
        return kFileRead.error();
    }
    Inode grown = kFileRead.value();

    unsigned long done = 0;
    while (done < count) {
        const unsigned long kFileBlock  = (offset + done) / kVolumeBlockBytes;
        const unsigned long kBlockShift = (offset + done) % kVolumeBlockBytes;
        const unsigned long kChunk = base::math::Min(kVolumeBlockBytes - kBlockShift, count - done);

        const base::Result<unsigned long> kDisk = ensure_data_block(grown, kFileBlock);
        if (!kDisk.ok()) {
            return kDisk.error();
        }
        if (!patch_storage(driver::Lba{.value = kDisk.value() * kBlockSectors}, kBlockShift, kChunk,
                           static_cast<const std::uint8_t*>(source) + done)) {
            return base::KernelError::kIOError;
        }
        done += kChunk;
    }

    grown.i_size =
        static_cast<std::uint32_t>(std::max<unsigned long>(grown.i_size, offset + count));
    grown.i_mtime = now_stamp();
    grown.i_ctime = grown.i_mtime;
    if (!write_inode(node.ino, grown)) {
        return base::KernelError::kIOError;
    }
    return count;
}

bool Ext2::bitmap_words(unsigned long bitmap_block, base::bit::WordMask* words,
                        unsigned long capacity) {
    return read_storage(driver::Lba{.value = bitmap_block * kBlockSectors}, words,
                        base::bit::Bitmap::words_for(capacity) * sizeof(base::bit::WordMask));
}

bool Ext2::patch_group_field(unsigned long group, unsigned long offset, std::uint16_t value) {
    const unsigned long kShift      = (group * sizeof(GroupDescriptor)) + offset;
    const unsigned long kTableBlock = sb_.s_first_data_block + 1 + (kShift / kVolumeBlockBytes);
    return patch_storage(driver::Lba{.value = kTableBlock * kBlockSectors},
                         kShift % kVolumeBlockBytes, sizeof(std::uint16_t), &value);
}

bool Ext2::adjust_free_blocks(unsigned long group, long delta) {
    if (delta < 0 && static_cast<long>(sb_.s_free_blocks_count) < -delta) {
        return false;
    }
    sb_.s_free_blocks_count = static_cast<std::uint32_t>(sb_.s_free_blocks_count + delta);
    groups_[group].bg_free_blocks_count =
        static_cast<std::uint16_t>(groups_[group].bg_free_blocks_count + delta);
    if (!patch_storage(driver::Lba{.value = kSuperblockSector},
                       offsetof(Superblock, s_free_blocks_count), sizeof(std::uint32_t),
                       &sb_.s_free_blocks_count)) {
        return false;
    }
    return patch_group_field(group, offsetof(GroupDescriptor, bg_free_blocks_count),
                             groups_[group].bg_free_blocks_count);
}

bool Ext2::adjust_free_inodes(unsigned long group, long delta) {
    if (delta < 0 && static_cast<long>(sb_.s_free_inodes_count) < -delta) {
        return false;
    }
    sb_.s_free_inodes_count = static_cast<std::uint32_t>(sb_.s_free_inodes_count + delta);
    groups_[group].bg_free_inodes_count =
        static_cast<std::uint16_t>(groups_[group].bg_free_inodes_count + delta);
    if (!patch_storage(driver::Lba{.value = kSuperblockSector},
                       offsetof(Superblock, s_free_inodes_count), sizeof(std::uint32_t),
                       &sb_.s_free_inodes_count)) {
        return false;
    }
    return patch_group_field(group, offsetof(GroupDescriptor, bg_free_inodes_count),
                             groups_[group].bg_free_inodes_count);
}

bool Ext2::adjust_used_dirs(unsigned long group, long delta) {
    groups_[group].bg_used_dirs_count =
        static_cast<std::uint16_t>(groups_[group].bg_used_dirs_count + delta);
    return patch_group_field(group, offsetof(GroupDescriptor, bg_used_dirs_count),
                             groups_[group].bg_used_dirs_count);
}

base::Result<unsigned long> Ext2::alloc_block() {
    for (unsigned long group = 0; group < group_count_; ++group) {
        if (groups_[group].bg_free_blocks_count == 0) {
            continue;
        }
        base::bit::WordMask words[kBitmapWords] = {};
        if (!bitmap_words(groups_[group].bg_block_bitmap, words, kBitsPerBitmapBlock)) {
            return base::KernelError::kIOError;
        }
        base::bit::Bitmap bits;
        bits.adopt(words, kBitsPerBitmapBlock);
        const unsigned long kBit = bits.find_clear_run(1);
        if (kBit == base::bit::kInvalidIndex || kBit >= sb_.s_blocks_per_group) {
            continue;
        }
        bits.set(kBit);
        const unsigned long kByte = kBit / 8;
        if (!patch_storage(driver::Lba{.value = groups_[group].bg_block_bitmap * kBlockSectors},
                           kByte, 1, byte_of(words, kByte)) ||
            !adjust_free_blocks(group, -1)) {
            return base::KernelError::kIOError;
        }
        return sb_.s_first_data_block + (group * sb_.s_blocks_per_group) + kBit;
    }
    return base::KernelError::kOutOfMemory;
}

base::Result<unsigned long> Ext2::alloc_inode(bool directory) {
    for (unsigned long group = 0; group < group_count_; ++group) {
        if (groups_[group].bg_free_inodes_count == 0) {
            continue;
        }
        base::bit::WordMask words[kBitmapWords] = {};
        if (!bitmap_words(groups_[group].bg_inode_bitmap, words, sb_.s_inodes_per_group)) {
            return base::KernelError::kIOError;
        }
        base::bit::Bitmap bits;
        bits.adopt(words, sb_.s_inodes_per_group);
        const unsigned long kBit = bits.find_clear_run(1);
        if (kBit == base::bit::kInvalidIndex || kBit >= sb_.s_inodes_per_group) {
            continue;
        }
        bits.set(kBit);
        const unsigned long kByte = kBit / 8;
        if (!patch_storage(driver::Lba{.value = groups_[group].bg_inode_bitmap * kBlockSectors},
                           kByte, 1, byte_of(words, kByte)) ||
            !adjust_free_inodes(group, -1)) {
            return base::KernelError::kIOError;
        }
        if (directory && !adjust_used_dirs(group, +1)) {
            return base::KernelError::kIOError;
        }
        return (group * sb_.s_inodes_per_group) + kBit + 1;
    }
    return base::KernelError::kOutOfMemory;
}

void Ext2::free_block(unsigned long block) {
    const unsigned long kPast               = block - sb_.s_first_data_block;
    const unsigned long kGroup              = kPast / sb_.s_blocks_per_group;
    const unsigned long kBit                = kPast % sb_.s_blocks_per_group;
    base::bit::WordMask words[kBitmapWords] = {};
    if (!bitmap_words(groups_[kGroup].bg_block_bitmap, words, kBitsPerBitmapBlock)) {
        return;
    }
    base::bit::Bitmap bits;
    bits.adopt(words, kBitsPerBitmapBlock);
    bits.clear(kBit);
    const unsigned long kByte = kBit / 8;
    static_cast<void>(
        patch_storage(driver::Lba{.value = groups_[kGroup].bg_block_bitmap * kBlockSectors}, kByte,
                      1, byte_of(words, kByte)));
    static_cast<void>(adjust_free_blocks(kGroup, +1));
}

void Ext2::free_inode(unsigned long ino) {
    const unsigned long kGroup = GroupOf(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    const unsigned long kBit =
        IndexOfInode(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    base::bit::WordMask words[kBitmapWords] = {};
    if (!bitmap_words(groups_[kGroup].bg_inode_bitmap, words, sb_.s_inodes_per_group)) {
        return;
    }
    base::bit::Bitmap bits;
    bits.adopt(words, sb_.s_inodes_per_group);
    bits.clear(kBit);
    const unsigned long kByte = kBit / 8;
    static_cast<void>(
        patch_storage(driver::Lba{.value = groups_[kGroup].bg_inode_bitmap * kBlockSectors}, kByte,
                      1, byte_of(words, kByte)));
    static_cast<void>(adjust_free_inodes(kGroup, +1));
}

void Ext2::free_direct_blocks(const Inode& node) {
    for (unsigned long slot = 0; slot < kDirectSlots; ++slot) {
        if (node.i_block[slot] != 0) {
            free_block(node.i_block[slot]);
        }
    }
}

void Ext2::free_single_indirect(const Inode& node) {
    if (node.i_block[kSingleSlot] == 0) {
        return;
    }
    std::uint32_t table[kBlockWords] = {};
    if (read_block(node.i_block[kSingleSlot], table)) {
        for (const unsigned int kIndex : table) {
            if (kIndex != 0) {
                free_block(kIndex);
            }
        }
    }
    free_block(node.i_block[kSingleSlot]);
}

void Ext2::free_double_indirect(const Inode& node) {
    if (node.i_block[kDoubleSlot] == 0) {
        return;
    }
    std::uint32_t outer[kBlockWords] = {};
    if (read_block(node.i_block[kDoubleSlot], outer)) {
        for (const unsigned long kFirst : outer) {
            if (kFirst == 0) {
                continue;
            }
            std::uint32_t inner[kBlockWords] = {};
            if (read_block(kFirst, inner)) {
                for (const unsigned long kSecond : inner) {
                    if (kSecond != 0) {
                        free_block(kSecond);
                    }
                }
            }
            free_block(kFirst);
        }
    }
    free_block(node.i_block[kDoubleSlot]);
}

void Ext2::free_data_blocks(const Inode& node) {
    free_direct_blocks(node);
    free_single_indirect(node);
    free_double_indirect(node);
}

bool Ext2::write_inode(unsigned long ino, const Inode& inode) {
    if (ino == 0 || ino > sb_.s_inodes_count) {
        return false;
    }
    const unsigned long kGroup = GroupOf(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    if (kGroup >= group_count_) {
        return false;
    }
    const unsigned long kIndex =
        IndexOfInode(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    const unsigned long kBytes = kIndex * sb_.s_inode_size;
    const unsigned long kBlock = groups_[kGroup].bg_inode_table + (kBytes / kVolumeBlockBytes);
    const unsigned long kShift = kBytes % kVolumeBlockBytes;
    return patch_storage(driver::Lba{.value = kBlock * kBlockSectors}, kShift, sizeof(Inode),
                         &inode);
}

base::Result<unsigned long> Ext2::zero_fresh_block() {
    base::Result<unsigned long> fresh = alloc_block();
    if (!fresh.ok()) {
        return fresh;
    }
    const std::uint8_t kZeroes[kVolumeBlockBytes] = {};
    if (!patch_storage(driver::Lba{.value = fresh.value() * kBlockSectors}, 0, kVolumeBlockBytes,
                       kZeroes)) {
        free_block(fresh.value());
        return base::KernelError::kIOError;
    }
    return fresh;
}

base::Result<unsigned long> Ext2::ensure_direct_block(Inode& node, unsigned long slot) {
    if (node.i_block[slot] != 0) {
        return static_cast<unsigned long>(node.i_block[slot]);
    }
    base::Result<unsigned long> fresh = zero_fresh_block();
    if (!fresh.ok()) {
        return fresh;
    }
    node.i_block[slot] = static_cast<std::uint32_t>(fresh.value());
    node.i_blocks      = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
    return fresh;
}

base::Result<unsigned long> Ext2::ensure_single_block(Inode& node, unsigned long outer) {
    if (node.i_block[kSingleSlot] == 0) {
        base::Result<unsigned long> table_block = zero_fresh_block();
        if (!table_block.ok()) {
            return table_block;
        }
        node.i_block[kSingleSlot] = static_cast<std::uint32_t>(table_block.value());
        node.i_blocks             = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
    }
    std::uint32_t     table[kBlockWords] = {};
    const driver::Lba kTableLba{.value = node.i_block[kSingleSlot] * kBlockSectors};
    if (!read_block(node.i_block[kSingleSlot], table)) {
        return base::KernelError::kIOError;
    }
    if (table[outer] == 0) {
        base::Result<unsigned long> fresh = zero_fresh_block();
        if (!fresh.ok()) {
            return fresh;
        }
        table[outer]  = static_cast<std::uint32_t>(fresh.value());
        node.i_blocks = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
        if (!patch_storage(kTableLba, outer * sizeof(std::uint32_t), sizeof(std::uint32_t),
                           &table[outer])) {
            return base::KernelError::kIOError;
        }
    }
    return static_cast<unsigned long>(table[outer]);
}

base::Result<unsigned long> Ext2::ensure_double_block(Inode& node, unsigned long outer,
                                                      unsigned long inner) {
    if (node.i_block[kDoubleSlot] == 0) {
        base::Result<unsigned long> outer_table = zero_fresh_block();
        if (!outer_table.ok()) {
            return outer_table;
        }
        node.i_block[kDoubleSlot] = static_cast<std::uint32_t>(outer_table.value());
        node.i_blocks             = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
    }
    std::uint32_t outer_words[kBlockWords] = {};
    if (!read_block(node.i_block[kDoubleSlot], outer_words)) {
        return base::KernelError::kIOError;
    }
    const driver::Lba kOuterLba{.value = node.i_block[kDoubleSlot] * kBlockSectors};
    if (outer_words[outer] == 0) {
        base::Result<unsigned long> inner_table = zero_fresh_block();
        if (!inner_table.ok()) {
            return inner_table;
        }
        outer_words[outer] = static_cast<std::uint32_t>(inner_table.value());
        node.i_blocks      = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
        if (!patch_storage(kOuterLba, outer * sizeof(std::uint32_t), sizeof(std::uint32_t),
                           &outer_words[outer])) {
            return base::KernelError::kIOError;
        }
    }
    std::uint32_t     inner_words[kBlockWords] = {};
    const driver::Lba kInnerLba{.value = outer_words[outer] * kBlockSectors};
    if (!read_block(outer_words[outer], inner_words)) {
        return base::KernelError::kIOError;
    }
    if (inner_words[inner] == 0) {
        base::Result<unsigned long> fresh = zero_fresh_block();
        if (!fresh.ok()) {
            return fresh;
        }
        inner_words[inner] = static_cast<std::uint32_t>(fresh.value());
        node.i_blocks      = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
        if (!patch_storage(kInnerLba, inner * sizeof(std::uint32_t), sizeof(std::uint32_t),
                           &inner_words[inner])) {
            return base::KernelError::kIOError;
        }
    }
    return static_cast<unsigned long>(inner_words[inner]);
}

base::Result<unsigned long> Ext2::ensure_data_block(Inode& node, unsigned long file_block) {
    const BlockSlot kSlot = MapFileBlock(file_block);
    switch (kSlot.kind) {
    case BlockSlot::Kind::kDirect:
        return ensure_direct_block(node, kSlot.slot);
    case BlockSlot::Kind::kSingle:
        return ensure_single_block(node, kSlot.outer);
    case BlockSlot::Kind::kDouble:
        return ensure_double_block(node, kSlot.outer, kSlot.inner);
    default:
        return base::KernelError::kNotSupported;
    }
}

bool Ext2::fill_dead_slot(const DirInsert& slot) {
    std::uint8_t fresh[kDirEntryHeadBytes + kFsNameMax] = {};
    entry_bytes(fresh, slot.child, slot.name);
    const unsigned long kNeed = DirEntryLength(slot.name.size());
    const unsigned long kRoom = slot.entry->rec_len - kNeed;
    const auto          kRecord =
        static_cast<std::uint16_t>(kRoom < kDirEntryHeadBytes ? slot.entry->rec_len : kNeed);
    base::WriteWord16(fresh, 4, kRecord);
    const driver::Lba kWhere{.value = slot.disk_block * kBlockSectors};
    if (!patch_storage(kWhere, slot.cursor, kNeed, fresh)) {
        return false;
    }
    if (kRoom >= kDirEntryHeadBytes) {
        std::uint8_t tail[kDirEntryHeadBytes] = {};
        base::WriteWord16(tail, 4, static_cast<std::uint16_t>(kRoom));
        return patch_storage(kWhere, slot.cursor + kNeed, sizeof(tail), tail);
    }
    return true;
}

bool Ext2::shrink_live_entry(const DirInsert& slot) {
    const unsigned long kEntryMin = DirEntryLength(slot.entry->name_len);
    const unsigned long kNeed     = DirEntryLength(slot.name.size());
    std::uint8_t        fresh[kDirEntryHeadBytes + kFsNameMax] = {};
    entry_bytes(fresh, slot.child, slot.name);
    const auto        kShrink = static_cast<std::uint16_t>(kEntryMin);
    const auto        kRemain = static_cast<std::uint16_t>(slot.entry->rec_len - kEntryMin);
    const driver::Lba kWhere{.value = slot.disk_block * kBlockSectors};
    if (!patch_storage(kWhere, slot.cursor + 4, sizeof(std::uint16_t), &kShrink) ||
        !patch_storage(kWhere, slot.cursor + kEntryMin, kNeed, fresh)) {
        return false;
    }
    return patch_storage(kWhere, slot.cursor + kEntryMin + 4, sizeof(std::uint16_t), &kRemain);
}

bool Ext2::entry_is_sound(const DirEntry& entry, unsigned long cursor) {
    return entry.rec_len >= kDirEntryHeadBytes && entry.rec_len % kDirEntryAlign == 0 &&
           cursor + kDirEntryHeadBytes + entry.name_len <= kVolumeBlockBytes &&
           kDirEntryHeadBytes + entry.name_len <= entry.rec_len;
}

bool Ext2::add_dir_entry(Inode& parent_dir, unsigned long parent_ino, unsigned long child_ino,
                         std::string_view name) {
    const unsigned long kNeed = DirEntryLength(name.size());
    const unsigned long kSpan = base::math::Ceil(parent_dir.i_size, kVolumeBlockBytes);
    std::uint32_t       block[kBlockWords];

    for (unsigned long file_block = 0; file_block < kSpan; ++file_block) {
        const base::Result<unsigned long> kDisk = resolve_data_block(parent_dir, file_block);
        if (!kDisk.ok() || kDisk.value() == 0) {
            continue;
        }
        if (!read_block(kDisk.value(), block)) {
            return false;
        }
        unsigned long cursor = 0;
        while (cursor + kDirEntryHeadBytes <= kVolumeBlockBytes) {
            const DirEntry& entry = *reinterpret_cast<const DirEntry*>(
                reinterpret_cast<const unsigned char*>(block) + cursor);
            if (!entry_is_sound(entry, cursor)) {
                return false;
            }
            const unsigned long kEntryMin = DirEntryLength(entry.name_len);
            const unsigned long kRoom     = entry.rec_len - kEntryMin;
            const DirInsert     kHere{
                .disk_block = kDisk.value(),
                .cursor     = cursor,
                .entry      = &entry,
                .child      = child_ino,
                .name       = name,
            };
            if (entry.inode == 0 && entry.rec_len >= kNeed) {
                return fill_dead_slot(kHere);
            }
            if (entry.inode != 0 && kRoom >= kNeed) {
                return shrink_live_entry(kHere);
            }
            cursor += entry.rec_len;
        }
    }
    const auto kDisk = ensure_data_block(parent_dir, kSpan);
    if (!kDisk.ok()) {
        return false;
    }
    std::uint8_t fresh[kDirEntryHeadBytes + kFsNameMax]{};
    entry_bytes(fresh, child_ino, name);
    base::WriteWord16(fresh, 4, static_cast<std::uint16_t>(kVolumeBlockBytes));
    if (!patch_storage(driver::Lba{.value = kDisk.value() * kBlockSectors}, 0, kNeed, fresh)) {
        return false;
    }
    parent_dir.i_size  = static_cast<std::uint32_t>((kSpan + 1) * kVolumeBlockBytes);
    parent_dir.i_mtime = now_stamp();
    parent_dir.i_ctime = parent_dir.i_mtime;
    return write_inode(parent_ino, parent_dir);
}

bool Ext2::replace_dir_entry_ino(const Inode& parent_dir, std::string_view name,
                                 unsigned long ino) {
    const unsigned long kSpan = base::math::Ceil(parent_dir.i_size, kVolumeBlockBytes);
    std::uint32_t       block[kBlockWords];

    for (unsigned long file_block = 0; file_block < kSpan; ++file_block) {
        const base::Result<unsigned long> kDisk = resolve_data_block(parent_dir, file_block);
        if (!kDisk.ok() || kDisk.value() == 0) {
            continue;
        }
        if (!read_block(kDisk.value(), block)) {
            return false;
        }
        unsigned long cursor = 0;
        while (cursor + kDirEntryHeadBytes <= kVolumeBlockBytes) {
            const DirEntry& entry = *reinterpret_cast<const DirEntry*>(
                reinterpret_cast<const unsigned char*>(block) + cursor);
            if (!entry_is_sound(entry, cursor)) {
                return false;
            }
            if (entry.inode != 0 && entry.name_len == name.size() &&
                base::EqualBytes(
                    reinterpret_cast<const unsigned char*>(&entry) + kDirEntryHeadBytes,
                    name.data(), name.size())) {
                return patch_storage(driver::Lba{.value = kDisk.value() * kBlockSectors}, cursor,
                                     sizeof(std::uint32_t), &ino);
            }
            cursor += entry.rec_len;
        }
    }
    return false;
}

base::Result<void> Ext2::drop_file_link(unsigned long ino) {
    const base::Result<Inode> kRead = read_inode(ino);
    if (!kRead.ok()) {
        return kRead.error();
    }
    Inode dead         = kRead.value();
    dead.i_links_count = static_cast<std::uint16_t>(dead.i_links_count - 1U);
    if (dead.i_links_count == 0 && !has_hold(ino)) {
        dead.i_dtime = clock_ == nullptr ? sb_.s_mkfs_time : now_stamp();
        free_data_blocks(dead);
    }
    if (!write_inode(ino, dead)) {
        return base::KernelError::kIOError;
    }
    if (dead.i_links_count == 0 && !has_hold(ino)) {
        free_inode(ino);
    }
    return {};
}

bool Ext2::remove_dir_entry(const Inode& parent_dir, std::string_view name) {
    const unsigned long kSpan = base::math::Ceil(parent_dir.i_size, kVolumeBlockBytes);
    std::uint32_t       block[kBlockWords];

    for (unsigned long file_block = 0; file_block < kSpan; ++file_block) {
        const base::Result<unsigned long> kDisk = resolve_data_block(parent_dir, file_block);
        if (!kDisk.ok() || kDisk.value() == 0) {
            continue;
        }
        if (!read_block(kDisk.value(), block)) {
            return false;
        }
        unsigned long cursor      = 0;
        unsigned long prev_cursor = 0;
        bool          has_prev    = false;
        while (cursor + kDirEntryHeadBytes <= kVolumeBlockBytes) {
            const DirEntry& entry = *reinterpret_cast<const DirEntry*>(
                reinterpret_cast<const unsigned char*>(block) + cursor);
            if (!entry_is_sound(entry, cursor)) {
                return false;
            }
            if (entry.inode != 0 && entry.name_len == name.size() &&
                base::EqualBytes(
                    reinterpret_cast<const unsigned char*>(&entry) + kDirEntryHeadBytes,
                    name.data(), name.size())) {
                if (has_prev) {
                    const DirEntry& prev = *reinterpret_cast<const DirEntry*>(
                        reinterpret_cast<const unsigned char*>(block) + prev_cursor);
                    const auto kMerged = static_cast<std::uint16_t>(prev.rec_len + entry.rec_len);
                    return patch_storage(driver::Lba{.value = kDisk.value() * kBlockSectors},
                                         prev_cursor + 4, sizeof(std::uint16_t), &kMerged);
                }
                const std::uint32_t kGone = 0;
                return patch_storage(driver::Lba{.value = kDisk.value() * kBlockSectors}, cursor,
                                     sizeof(std::uint32_t), &kGone);
            }
            if (entry.inode != 0) {
                prev_cursor = cursor;
                has_prev    = true;
            }
            cursor += entry.rec_len;
        }
    }
    return false;
}

}  // namespace cinux::fs::ext2
