/**
 * @file ext2_mutation.hpp
 * @brief Private rollback scope for volume metadata and unpublished cache pages.
 * @ingroup kernel_fs
 */
#pragma once
#include "cinux/own.hpp"
#include "kernel/driver/block/block_cache.hpp"
#include "kernel/fs/ext2/ext2.hpp"
namespace cinux::fs::ext2 {
/** @brief Own unpublished metadata changes while the volume mutex is held. */
class Ext2::Mutation {
public:
    explicit Mutation(Ext2& volume);
    ~Mutation();
    Mutation(const Mutation&)            = delete;
    Mutation& operator=(const Mutation&) = delete;
    /** @brief Publish the batch. @return False without publication on preparation failure. */
    bool      commit();
    /** @brief Read through staged pages. @return Whether the read completed. */
    bool      read(driver::Lba first, void* sink, unsigned long bytes) {
        return batch_.read(first, sink, bytes);
    }
    /** @brief Patch a private copy. @return Whether the change was staged. */
    bool patch(driver::Lba first, unsigned long offset, unsigned long bytes, const void* source) {
        return batch_.patch(first, offset, bytes, source);
    }

private:
    struct State {
        Superblock      superblock;
        GroupDescriptor groups[kMaxGroups];
    };
    Ext2&                     volume_;
    driver::BlockCache::Batch batch_;
    base::Own<State>          before_;
    bool                      committed_ = false;
};
}
