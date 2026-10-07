/**
 * @file    ramfs.cpp
 * @brief   The ramfs payload: heap-grown file bytes on a named tree.
 *
 * The climb lives in dir_tree.hpp — this file only owns what makes
 * ramfs ramfs: the byte buffer of each file, its growth, its zeroed
 * gaps, and the one spinlock held across every operation. File bytes
 * are owned through unique_ptr, so buffer and node end together; the
 * each parent-child edge owns one node reference, as does each open
 * descriptor. Removing a name drops the tree owner; the last owner
 * destroys the node. The chain is tended by the tree.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/ramfs/ramfs.hpp"

#include <memory>
#include <string_view>

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/path.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/dir_tree.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/ramfs/ramfs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs {

/**
 * @brief   One ramfs node: the tree half plus the byte-buffer half.
 * @note    File-private on purpose; handles going out carry FsNode.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct RamNode : DirNode<RamNode> {
    std::unique_ptr<unsigned char[]> data;
    unsigned long                    capacity = 0;
    unsigned long                    size     = 0;
    base::RefCount<>                 references;

    void release_ref() {
        if (references.release()) {
            delete this;
        }
    }
};

namespace {

bool name_is_new(std::string_view name) {
    return !name.empty() && name.size() + 1 <= kFsNameMax && !name.contains('/');
}

base::Result<RamNode*> open_dir(RamNode& root, std::string_view dir_path) {
    const base::Result<RamNode*> kFound = WalkPath(root, dir_path);
    if (!kFound.ok()) {
        return kFound.error();
    }
    RamNode* const kDir = kFound.value();
    if (kDir->type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }
    return kDir;
}

base::Result<RamNode*> make_node(RamNode& root, unsigned long& next_ino, std::string_view path,
                                 InodeType type) {
    std::string_view            parent_path;
    std::string_view            leaf;
    [[maybe_unused]] const bool kHadSlash = base::SplitLeaf(path, parent_path, leaf);
    if (!name_is_new(leaf)) {
        return base::KernelError::kInvalidArgument;
    }
    const base::Result<RamNode*> kDir = open_dir(root, parent_path);
    if (!kDir.ok()) {
        return kDir.error();
    }
    if (kDir.value()->find(leaf) != nullptr) {
        return base::KernelError::kAlreadyExists;
    }

    auto* const kNode = new RamNode{};
    base::CopyBytes(kNode->name, leaf.data(), leaf.size());
    kNode->type = type;
    kNode->ino  = next_ino++;
    kDir.value()->adopt(*kNode);
    return kNode;
}

void destroy_tree(RamNode* node) {
    while (node->first_child != nullptr) {
        RamNode* const kChild = node->first_child;
        node->first_child     = kChild->next_sibling;
        destroy_tree(kChild);
    }
    node->release_ref();
}

FsNode node_handle(RamNode& node) {
    return FsNode{.opaque = &node, .type = node.type, .ino = node.ino, .size = node.size};
}

}  // namespace

RamFs::RamFs() : root_(new RamNode{}) {
    root_->type = InodeType::kDirectory;
    root_->ino  = 1;
}

RamFs::~RamFs() {
    lock_.retire();
    destroy_tree(root_);
}

base::Result<FsNode> RamFs::lookup(std::string_view path) {
    const proc::SpinGuard        kGuard(lock_);
    const base::Result<RamNode*> kFound = WalkPath(*root_, path);
    if (!kFound.ok()) {
        return kFound.error();
    }
    return node_handle(*kFound.value());
}

base::Result<FsNode> RamFs::make_file(std::string_view path) {
    const proc::SpinGuard        kGuard(lock_);
    const base::Result<RamNode*> kMade = make_node(*root_, next_ino_, path, InodeType::kFile);
    if (!kMade.ok()) {
        return kMade.error();
    }
    return node_handle(*kMade.value());
}

base::Result<FsNode> RamFs::make_dir(std::string_view path) {
    const proc::SpinGuard        kGuard(lock_);
    const base::Result<RamNode*> kMade = make_node(*root_, next_ino_, path, InodeType::kDirectory);
    if (!kMade.ok()) {
        return kMade.error();
    }
    return node_handle(*kMade.value());
}

base::Result<void> RamFs::unlink(std::string_view path) {
    const proc::SpinGuard kGuard(lock_);

    std::string_view            parent_path;
    std::string_view            leaf;
    [[maybe_unused]] const bool kHadSlash = base::SplitLeaf(path, parent_path, leaf);
    if (leaf.empty()) {
        return base::KernelError::kInvalidArgument;
    }

    const base::Result<RamNode*> kDirResult = WalkPath(*root_, parent_path);
    if (!kDirResult.ok()) {
        return kDirResult.error();
    }
    RamNode* const kDir = kDirResult.value();
    if (kDir->type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }

    RamNode* const kVictim = kDir->disown(leaf);
    if (kVictim == nullptr) {
        return base::KernelError::kNotFound;
    }
    if (kVictim->type == InodeType::kDirectory && kVictim->first_child != nullptr) {
        kDir->adopt(*kVictim);
        return base::KernelError::kDirectoryNotEmpty;
    }
    kVictim->release_ref();
    return {};
}

void RamFs::retain(FsNode node) {
    const proc::SpinGuard kGuard(lock_);
    auto* const           kNode = static_cast<RamNode*>(node.opaque);
    kNode->references.retain();
}

void RamFs::release(FsNode node) {
    const proc::SpinGuard kGuard(lock_);
    auto* const           kNode = static_cast<RamNode*>(node.opaque);
    kNode->release_ref();
}

base::Result<unsigned long> RamFs::read(FsNode node, unsigned long offset, void* sink,
                                        unsigned long count) {
    const proc::SpinGuard kGuard(lock_);
    auto* const           kFile = static_cast<RamNode*>(node.opaque);
    if (kFile->type != InodeType::kFile) {
        return base::KernelError::kIsADirectory;
    }
    if (offset >= kFile->size) {
        return 0UL;
    }
    const unsigned long kBytes = base::math::Min(count, kFile->size - offset);
    base::CopyBytes(sink, kFile->data.get() + offset, kBytes);
    return kBytes;
}

base::Result<unsigned long> RamFs::write(FsNode node, unsigned long offset, const void* source,
                                         unsigned long count) {
    const proc::SpinGuard kGuard(lock_);
    auto* const           kFile = static_cast<RamNode*>(node.opaque);
    if (kFile->type != InodeType::kFile) {
        return base::KernelError::kIsADirectory;
    }
    if (count == 0) {
        return 0UL;
    }

    const unsigned long kNeeded = offset + count;
    if (kNeeded > kFile->capacity) {
        const unsigned long kFresh =
            base::math::Ceil(kNeeded, kRamfsGrowthAlign) * kRamfsGrowthAlign;
        auto* const kGrown = new unsigned char[kFresh]{};
        if (kFile->size > 0) {
            base::CopyBytes(kGrown, kFile->data.get(), kFile->size);
        }
        kFile->data.reset(kGrown);
        kFile->capacity = kFresh;
    } else if (offset > kFile->size) {
        base::SetBytes(kFile->data.get() + kFile->size, 0, offset - kFile->size);
    }

    base::CopyBytes(kFile->data.get() + offset, source, count);
    kFile->size = base::math::Max(kFile->size, kNeeded);
    return count;
}

base::Result<bool> RamFs::read_dir(FsNode node, unsigned long index, FsDirent& entry) {
    const proc::SpinGuard kGuard(lock_);
    auto* const           kDir = static_cast<RamNode*>(node.opaque);
    if (kDir->type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }

    RamNode* const kChild = kDir->child_at(index);
    if (kChild == nullptr) {
        return false;
    }
    base::CopyBytes(entry.name, kChild->name, kFsNameMax);
    entry.type = kChild->type;
    return true;
}

}  // namespace cinux::fs
