/**
 * @file    ext2.cpp
 * @brief   Walking the volume: superblock, groups, inodes, directory
 *          entries, all through the queue.
 *
 * Every on-disk view lands in one 1 KiB word-aligned scratch on the
 * caller's stack, so no two asks ever share a buffer — the shared-
 * scratch sickness the second act diagnosed cannot get a foothold
 * here, per-call from day one. Both directory questions — find this
 * name, hand me the n-th entry — ride one walk_entries skeleton, so
 * the rec_len stepping rules live in exactly one place, and every
 * entry is checked against the block before its bytes are touched:
 * a damaged volume is refused loudly, never half-read. No lock lives
 * here: mount owns the volume single-handed in its bring-up task,
 * and the post-mount snapshot is read-only forever after.
 *
 * @author  Charliechen114514
 * @date    2026-10-10
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/ext2/ext2.hpp"

#include <cstdint>
#include <string_view>

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/path.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/block/block_cache.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/fs/ext2/ext2_layout.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::fs::ext2 {

namespace {

const char* name_bytes_of(const DirEntry& entry) {
    return reinterpret_cast<const char*>(&entry) + kDirEntryHeadBytes;
}

[[nodiscard]] bool name_matches(const DirEntry& entry, std::string_view name) {
    return entry.name_len == name.size() &&
           base::EqualBytes(name_bytes_of(entry), name.data(), name.size());
}

}  // namespace

Ext2::Ext2(driver::BlockCache& cache, ClockFn clock) : cache_(cache), clock_(clock) {}

Ext2::~Ext2() {
    while (const auto* hold = held_.pop_head()) {
        delete hold;
    }
    write_gate_.retire();
}

bool Ext2::sync() {
    const proc::MutexGuard kGate(write_gate_);
    return mounted_ && reap_pending() && cache_.flush();
}

base::Result<void> Ext2::mount() {
    if (mounted_) {
        return base::KernelError::kInvalidArgument;
    }

    if (!read_storage(driver::Lba{.value = kSuperblockSector}, &sb_, sizeof(Superblock))) {
        return base::KernelError::kIOError;
    }
    if (sb_.s_magic != kMagic) {
        return base::KernelError::kIOError;
    }
    if (sb_.s_log_block_size != 0 || sb_.s_feature_incompat != 0 ||
        sb_.s_inode_size < sizeof(Inode) || kVolumeBlockBytes % sb_.s_inode_size != 0) {
        return base::KernelError::kNotSupported;
    }
    if (sb_.s_first_data_block != 1 || sb_.s_blocks_count <= sb_.s_first_data_block ||
        sb_.s_blocks_per_group == 0 || sb_.s_blocks_per_group > kVolumeBlockBytes * 8 ||
        sb_.s_inodes_per_group == 0) {
        return base::KernelError::kIOError;
    }

    block_size_ = DecodeBlockSize(sb_.s_log_block_size);
    group_count_ =
        base::math::Ceil(sb_.s_blocks_count - sb_.s_first_data_block, sb_.s_blocks_per_group);
    if (group_count_ > kMaxGroups) {
        return base::KernelError::kNotSupported;
    }
    if (base::math::Ceil(sb_.s_inodes_count, sb_.s_inodes_per_group) > group_count_) {
        return base::KernelError::kIOError;
    }

    const unsigned long kDescriptorsPerBlock = kVolumeBlockBytes / sizeof(GroupDescriptor);
    const unsigned long kTableBlocks         = base::math::Ceil(group_count_, kDescriptorsPerBlock);
    for (unsigned long block = 0; block < kTableBlocks; ++block) {
        if (!read_block(sb_.s_first_data_block + 1 + block,
                        groups_ + (block * kDescriptorsPerBlock))) {
            return base::KernelError::kIOError;
        }
    }

    const base::Result<Inode> kRoot = read_inode(kRootInode);
    if (!kRoot.ok()) {
        return kRoot.error();
    }
    if (!IsDirectoryMode(kRoot.value().i_mode)) {
        return base::KernelError::kIOError;
    }

    mounted_ = true;
    return {};
}

struct Ext2::SymlinkPlan {
    std::string_view path;
    unsigned long    parent_bytes;
    std::string_view rest;
    char*            pending;
};

base::Result<void> Ext2::expand_symlink(const Inode& inode, unsigned long ino,
                                        const SymlinkPlan& plan) {
    char       target[kFsPathMax]{};
    const auto kBytes = inode.i_size;
    if (kBytes == 0 || kBytes >= sizeof(target)) {
        return base::KernelError::kInvalidArgument;
    }
    if (kBytes <= sizeof(inode.i_block) && inode.i_blocks == 0) {
        base::CopyBytes(target, inode.i_block, kBytes);
    } else {
        const FsNode kLink{
            .opaque = nullptr,
            .type   = InodeType::kFile,
            .ino    = ino,
            .size   = kBytes,
        };
        const auto kRead = read_locked(kLink, 0, target, kBytes);
        if (!kRead.ok() || kRead.value() != kBytes) {
            return base::KernelError::kIOError;
        }
    }
    char                   expanded[kFsPathMax]{};
    const std::string_view kParent = plan.path.substr(0, plan.parent_bytes);
    const auto             kFold = base::FoldOntoPath(kParent, target, expanded, sizeof(expanded));
    if (!kFold.ok || kFold.length + plan.rest.size() >= kFsPathMax) {
        return base::KernelError::kInvalidArgument;
    }
    base::CopyBytes(expanded + kFold.length, plan.rest.data(), plan.rest.size());
    base::CopyBytes(plan.pending, expanded, kFold.length + plan.rest.size());
    plan.pending[kFold.length + plan.rest.size()] = '\0';
    return {};
}

base::Result<Inode> Ext2::read_child(const Inode& parent, std::string_view name,
                                     unsigned long& ino) {
    if (!IsDirectoryMode(parent.i_mode)) {
        return base::KernelError::kNotADirectory;
    }
    if (name == ".") {
        return parent;
    }
    if (name == ".." && ino == kRootInode) {
        return base::KernelError::kNotSupported;
    }
    const auto kChild = lookup_in_dir(parent, name);
    if (!kChild.ok()) {
        return kChild.error();
    }
    ino = kChild.value();
    return read_inode(ino);
}

base::Result<FsNode> Ext2::lookup(std::string_view path) {
    const proc::MutexGuard kGate(write_gate_);
    return lookup_locked(path);
}
base::Result<FsNode> Ext2::lookup_held(std::string_view path) {
    const proc::MutexGuard kGate(write_gate_);
    const auto             kNode = lookup_locked(path);
    if (kNode.ok()) {
        retain_locked(kNode.value());
    }
    return kNode;
}
base::Result<FsNode> Ext2::lookup_locked(std::string_view path) {
    if (!mounted_) {
        return base::KernelError::kIOError;
    }

    char pending[kFsPathMax]{};
    if (path.size() >= sizeof(pending)) {
        return base::KernelError::kInvalidArgument;
    }
    base::CopyBytes(pending, path.data(), path.size());
    return walk_from(kRootInode, pending);
}

base::Result<FsNode> Ext2::lookup_at(FsNode base, std::string_view rest) {
    const proc::MutexGuard kGate(write_gate_);
    if (!mounted_) {
        return base::KernelError::kIOError;
    }
    if (base.type != InodeType::kDirectory || rest.empty() || rest.front() == '/') {
        return base::KernelError::kInvalidArgument;
    }
    char pending[kFsPathMax]{};
    if (rest.size() >= sizeof(pending)) {
        return base::KernelError::kInvalidArgument;
    }
    base::CopyBytes(pending, rest.data(), rest.size());
    const auto kAnchor = read_inode(base.ino);
    if (!kAnchor.ok()) {
        return kAnchor.error();
    }
    if (kAnchor.value().i_links_count == 0) {
        return base::KernelError::kNotFound;
    }
    const auto kNode = walk_from(base.ino, pending);
    if (kNode.ok()) {
        retain_locked(kNode.value());
    }
    return kNode;
}

base::Result<bool> Ext2::redirect_through_link(const LinkSpot& spot, const Inode& link,
                                               unsigned long ino) {
    const auto kParentBytes = static_cast<unsigned long>(spot.part.data() - spot.walked.data());
    const auto kExpanded    = expand_symlink(link, ino,
                                             {
                                                 .path         = spot.walked,
                                                 .parent_bytes = kParentBytes,
                                                 .rest         = spot.rest,
                                                 .pending      = spot.pending,
                                             });
    if (!kExpanded.ok()) {
        return kExpanded.error();
    }
    if (spot.start_ino != kRootInode && spot.pending[0] == '/') {
        return base::KernelError::kNotSupported;
    }
    return true;
}

base::Result<FsNode> Ext2::walk_from(unsigned long start_ino, char* pending) {
    for (unsigned int followed = 0; followed <= 40; ++followed) {
        unsigned long current_ino = start_ino;
        auto          current     = read_inode(current_ino);
        if (!current.ok()) {
            return current.error();
        }
        const std::string_view kPath{pending};
        base::PathCursor       walk{kPath};
        std::string_view       part;
        bool                   redirected = false;
        while (base::NextComponent(walk, part)) {
            current = read_child(current.value(), part, current_ino);
            if (!current.ok()) {
                return current.error();
            }
            if (!IsSymlinkMode(current.value().i_mode)) {
                continue;
            }
            const auto kLink = redirect_through_link(
                {
                    .walked    = kPath,
                    .part      = part,
                    .rest      = walk.rest,
                    .pending   = pending,
                    .start_ino = start_ino,
                },
                current.value(), current_ino);
            if (!kLink.ok()) {
                return kLink.error();
            }
            redirected = true;
            break;
        }
        if (!redirected) {
            const bool kDirectory = IsDirectoryMode(current.value().i_mode);
            return FsNode{
                .opaque = nullptr,
                .type   = kDirectory ? InodeType::kDirectory : InodeType::kFile,
                .ino    = current_ino,
                .size   = kDirectory ? 0 : current.value().i_size,
            };
        }
    }
    return base::KernelError::kInvalidArgument;
}

base::Result<unsigned long> Ext2::read(FsNode node, unsigned long offset, void* sink,
                                       unsigned long count) {
    const proc::MutexGuard kGate(write_gate_);
    return read_locked(node, offset, sink, count);
}
base::Result<unsigned long> Ext2::read_locked(FsNode node, unsigned long offset, void* sink,
                                              unsigned long count) {
    if (!mounted_) {
        return base::KernelError::kIOError;
    }
    if (node.type == InodeType::kDirectory) {
        return base::KernelError::kIsADirectory;
    }

    const base::Result<Inode> kFile = read_inode(node.ino);
    if (!kFile.ok()) {
        return kFile.error();
    }
    if (offset >= kFile.value().i_size) {
        return 0UL;
    }

    const unsigned long kWant = base::math::Min(count, kFile.value().i_size - offset);
    unsigned long       done  = 0;
    std::uint32_t       block[kBlockWords];
    while (done < kWant) {
        const unsigned long kFileBlock  = (offset + done) / block_size_;
        const unsigned long kBlockShift = (offset + done) % block_size_;
        const unsigned long kChunk      = base::math::Min(block_size_ - kBlockShift, kWant - done);

        const base::Result<unsigned long> kDisk = resolve_data_block(kFile.value(), kFileBlock);
        if (!kDisk.ok()) {
            return kDisk.error();
        }
        if (kDisk.value() == 0) {
            base::SetBytes(static_cast<unsigned char*>(sink) + done, 0, kChunk);
        } else {
            if (!read_block(kDisk.value(), block)) {
                return base::KernelError::kIOError;
            }
            base::CopyBytes(static_cast<unsigned char*>(sink) + done,
                            reinterpret_cast<const unsigned char*>(block) + kBlockShift, kChunk);
        }
        done += kChunk;
    }
    return done;
}

base::Result<bool> Ext2::read_dir(FsNode node, unsigned long index, FsDirent& entry) {
    const proc::MutexGuard kGate(write_gate_);
    if (!mounted_) {
        return base::KernelError::kIOError;
    }
    if (node.type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }

    const base::Result<Inode> kDir = read_inode(node.ino);
    if (!kDir.ok()) {
        return kDir.error();
    }

    unsigned long            seen      = 0;
    unsigned long            found_ino = 0;
    const base::Result<bool> kWalk     = walk_entries(kDir.value(), [&](const DirEntry& item) {
        if (item.name_len >= kFsNameMax) {
            return true;
        }
        if (seen != index) {
            ++seen;
            return true;
        }
        base::CopyBytes(entry.name, name_bytes_of(item), item.name_len);
        entry.name[item.name_len] = '\0';
        found_ino                 = item.inode;
        return false;
    });
    if (!kWalk.ok()) {
        return kWalk.error();
    }
    if (found_ino == 0) {
        return false;
    }

    const base::Result<Inode> kChild = read_inode(found_ino);
    if (!kChild.ok()) {
        return kChild.error();
    }
    entry.type = IsDirectoryMode(kChild.value().i_mode) ? InodeType::kDirectory : InodeType::kFile;
    entry.ino  = found_ino;
    return true;
}

bool Ext2::readable(FsNode /*node*/) {
    return true;
}

long long Ext2::ioctl(FsNode /*node*/, unsigned long long /*word*/,
                      unsigned long long /*argument*/) {
    return -syscall::kEnotty;
}

base::Result<FsNode> Ext2::adopt_open(FsNode node, std::string_view /*path*/) {
    retain(node);
    return node;
}

base::Result<void> Ext2::stat(FsNode node, FsStat& probed) {
    const proc::MutexGuard kGate(write_gate_);
    if (!mounted_) {
        return base::KernelError::kIOError;
    }
    const base::Result<Inode> kRead = read_inode(node.ino);
    if (!kRead.ok()) {
        return kRead.error();
    }
    const Inode& on_disk = kRead.value();
    probed.ino           = node.ino;
    probed.mode          = on_disk.i_mode;
    probed.links         = on_disk.i_links_count;
    probed.size          = on_disk.i_size;
    probed.mtime         = on_disk.i_mtime;
    return {};
}

std::uint32_t Ext2::now_stamp() {
    return clock_ == nullptr ? std::uint32_t{0} : static_cast<std::uint32_t>((*clock_)());
}

bool Ext2::read_block(unsigned long block, void* sink) {
    return read_storage(driver::Lba{.value = block * kBlockSectors}, sink, kVolumeBlockBytes);
}

base::Result<Inode> Ext2::read_inode(unsigned long ino) {
    if (ino == 0 || ino > sb_.s_inodes_count) {
        return base::KernelError::kInvalidArgument;
    }
    const unsigned long kGroup = GroupOf(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    if (kGroup >= group_count_) {
        return base::KernelError::kIOError;
    }

    const unsigned long kIndex =
        IndexOfInode(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    const unsigned long kBytes = kIndex * sb_.s_inode_size;
    const unsigned long kBlock = groups_[kGroup].bg_inode_table + (kBytes / block_size_);
    const unsigned long kShift = kBytes % block_size_;

    std::uint32_t raw[kBlockWords];
    if (!read_block(kBlock, raw)) {
        return base::KernelError::kIOError;
    }
    Inode inode{};
    base::CopyBytes(&inode, reinterpret_cast<const unsigned char*>(raw) + kShift, sizeof(Inode));
    return inode;
}

base::Result<unsigned long> Ext2::resolve_data_block(const Inode& node, unsigned long file_block) {
    const BlockSlot kSlot = MapFileBlock(file_block);
    switch (kSlot.kind) {
    case BlockSlot::Kind::kDirect:
        return static_cast<unsigned long>(node.i_block[kSlot.slot]);

    case BlockSlot::Kind::kSingle: {
        if (node.i_block[kSingleSlot] == 0) {
            return 0UL;
        }
        std::uint32_t table[kBlockWords];
        if (!read_block(node.i_block[kSingleSlot], table)) {
            return base::KernelError::kIOError;
        }
        return static_cast<unsigned long>(table[kSlot.outer]);
    }

    case BlockSlot::Kind::kDouble: {
        if (node.i_block[kDoubleSlot] == 0) {
            return 0UL;
        }
        std::uint32_t outer[kBlockWords];
        if (!read_block(node.i_block[kDoubleSlot], outer)) {
            return base::KernelError::kIOError;
        }
        if (outer[kSlot.outer] == 0) {
            return 0UL;
        }
        std::uint32_t inner[kBlockWords];
        if (!read_block(outer[kSlot.outer], inner)) {
            return base::KernelError::kIOError;
        }
        return static_cast<unsigned long>(inner[kSlot.inner]);
    }

    default:
        return base::KernelError::kNotSupported;
    }
}

base::Result<unsigned long> Ext2::lookup_in_dir(const Inode& directory, std::string_view name) {
    unsigned long            matched = 0;
    const base::Result<bool> kWalk   = walk_entries(directory, [&](const DirEntry& item) -> bool {
        if (item.inode != 0 && name_matches(item, name)) {
            matched = item.inode;
            return false;
        }
        return true;
    });
    if (!kWalk.ok()) {
        return kWalk.error();
    }
    if (matched == 0) {
        return base::KernelError::kNotFound;
    }
    return matched;
}

static_assert(cinux::fs::FileSystem<Ext2>, "the ext2 backend satisfies the FileSystem face");

}  // namespace cinux::fs::ext2
