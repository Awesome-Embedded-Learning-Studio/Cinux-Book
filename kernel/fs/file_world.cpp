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
#include "cinux/shared.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/block/block_cache.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/rtc/rtc.hpp"
#include "kernel/driver/usb/usb_keyboard.hpp"
#include "kernel/fs/devfs/console_node.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/devfs/pty_node.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/procfs/procfs_init.hpp"
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

    driver::DeviceModel::self().bind(devfs_);
    RegisterBuiltinDevices();
    RegisterConsoleDevice();
    RegisterPtyDevices();
    [[maybe_unused]] const bool kUsbKeyboard = driver::SeatUsbKeyboard();
    const base::Result<void>    kSata        = driver::DeviceModel::self().seat({
        .klass       = driver::DeviceClass::kBlock,
        .name        = "sata0",
        .number      = MakeDeviceId(8, 0),
        .ops         = nullptr,
        .bus         = "pci",
        .driver_name = "ahci",
    });
    base::safety::Check(kSata.ok(), "device model refused the sata0 seat");
    const base::Result<void> kDevices = vfs_.mount("/dev", devfs_);
    base::safety::Check(kDevices.ok(), "devfs mount failed");

    WireKernelProcSource(procfs_);
    const base::Result<void> kFacts = vfs_.mount("/proc", procfs_);
    base::safety::Check(kFacts.ok(), "procfs mount failed");
}

base::Result<FsView> FileWorld::resolve(std::string_view path) {
    return vfs_.resolve(path);
}

base::Result<void> FileWorld::seat_console_streams(FileTable& table) {
    const base::Result<FsView> kView = vfs_.resolve("/dev/console");
    if (!kView.ok()) {
        return kView.error();
    }
    const FsView               kFlat = kView.value();
    const base::Result<FsNode> kNode = kFlat.ops->lookup(kFlat.self, "console");
    if (!kNode.ok()) {
        return kNode.error();
    }
    kFlat.ops->retain(kFlat.self, kNode.value());
    const base::Shared<OpenFile> kStream{new OpenFile(kFlat.ops, kFlat.self, kNode.value())};
    if (!kStream.holds()) {
        kFlat.ops->release(kFlat.self, kNode.value());
        return base::KernelError::kOutOfMemory;
    }
    kStream->status_flags = kFsReadWriteStatus;
    for (unsigned long seat = 0; seat < 3; ++seat) {
        const base::Result<unsigned long> kSeated = table.adopt_at(kStream, seat);
        if (!kSeated.ok()) {
            return kSeated.error();
        }
    }
    return {};
}

}  // namespace cinux::fs
