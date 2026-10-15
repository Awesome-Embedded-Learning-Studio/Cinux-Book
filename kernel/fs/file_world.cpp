/**
 * @file    file_world.cpp
 * @brief   Wiring the world: one queue, one volume, one root mount.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.3
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/file_world.hpp"

#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/block/block_cache.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/driver/rtc/rtc.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

namespace {

unsigned long long wall_clock() {
    return driver::Rtc::self().now_seconds();
}

}  // namespace

void FileWorld::init() {
    queue_  = new driver::BlockQueue(driver::Ahci::self());
    cache_  = new driver::BlockCache(*queue_, driver::kBlockCacheSlots);
    volume_ = new ext2::Ext2(*cache_, wall_clock);

    const base::Result<void> kMounted = volume_->mount();
    base::safety::Check(kMounted.ok(), "ext2 volume refused to mount");
    const base::Result<void> kRoot = vfs_.mount("/", *volume_);
    base::safety::Check(kRoot.ok(), "ext2 root mount failed");
}

base::Result<FsView> FileWorld::resolve(std::string_view path) {
    return vfs_.resolve(path);
}

}  // namespace cinux::fs
