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
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/block/block_cache.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/nvme/nvme.hpp"
#include "kernel/driver/rtc/rtc.hpp"
#include "kernel/driver/usb/usb_keyboard.hpp"
#include "kernel/fs/devfs/console_node.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/devfs/pty_node.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/mount_lease.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/procfs/procfs_init.hpp"
#include "kernel/fs/ramfs/ramfs.hpp"
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
    const base::Result<void> kRoot = vfs_.mount("/", *volume_, "ext2");
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
    if (driver::Nvme::self().ready()) {
        const auto kNvme = driver::DeviceModel::self().seat({
            .klass       = driver::DeviceClass::kBlock,
            .name        = "nvme0",
            .number      = MakeDeviceId(259, 0),
            .ops         = nullptr,
            .bus         = "pci",
            .driver_name = "nvme",
        });
        base::safety::Check(kNvme.ok(), "device model refused the nvme0 seat");
    }
    const base::Result<void> kDevices = vfs_.mount("/dev", devfs_, "devtmpfs");
    base::safety::Check(kDevices.ok(), "devfs mount failed");

    WireKernelProcSource(procfs_);
    const base::Result<void> kFacts = vfs_.mount("/proc", procfs_, "proc");
    base::safety::Check(kFacts.ok(), "procfs mount failed");
}

base::Result<FsView> FileWorld::resolve(std::string_view path) {
    return vfs_.resolve(path);
}

base::Result<void> FileWorld::mount_tmpfs(std::string_view target) {
    const auto kGuard = mount_gate_.guard();
    const auto kView  = vfs_.resolve(target);
    if (!kView.ok()) {
        return kView.error();
    }
    const auto kNode = kView.value().ops->lookup(kView.value().self, kView.value().rest);
    if (!kNode.ok()) {
        return kNode.error();
    }
    if (kNode.value().type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }
    for (TmpFsSeat& seat : tmpfs_seats_) {
        if (seat.volume != nullptr) {
            continue;
        }
        seat.volume = new RamFs();
        if (seat.volume == nullptr) {
            return base::KernelError::kOutOfMemory;
        }
        seat.lifetime = new MountLease{};
        if (seat.lifetime == nullptr) {
            delete seat.volume;
            seat.volume = nullptr;
            return base::KernelError::kOutOfMemory;
        }
        seat.lifetime->backend = seat.volume;
        seat.lifetime->destroy = [](void* backend) { delete static_cast<RamFs*>(backend); };
        const base::Result<void> kSeated = vfs_.mount(target, *seat.volume, "tmpfs", seat.lifetime);
        if (!kSeated.ok()) {
            if (seat.lifetime->release()) {
                delete seat.lifetime;
            }
            seat.lifetime = nullptr;
            seat.volume   = nullptr;
            return kSeated.error();
        }
        seat.prefix_len = target.size();
        base::CopyBytes(seat.prefix, target.data(), target.size());
        return {};
    }
    return base::KernelError::kOutOfMemory;
}

base::Result<void> FileWorld::umount_tmpfs(std::string_view target, bool lazy) {
    const auto kGuard = mount_gate_.guard();
    for (TmpFsSeat& seat : tmpfs_seats_) {
        if (seat.volume == nullptr || std::string_view(seat.prefix, seat.prefix_len) != target) {
            continue;
        }
        if (!lazy && seat.lifetime->use_count() > 1) {
            return base::KernelError::kWouldBlock;
        }
        const base::Result<void> kGone = vfs_.unmount(target);
        if (!kGone.ok()) {
            return kGone.error();
        }
        if (seat.lifetime->release()) {
            delete seat.lifetime;
        }
        seat.lifetime   = nullptr;
        seat.volume     = nullptr;
        seat.prefix_len = 0;
        return {};
    }
    return base::KernelError::kNotFound;
}

base::Result<void> FileWorld::seat_console_streams(FileTable& table) {
    const base::Result<FsView> kView = vfs_.resolve("/dev/console");
    if (!kView.ok()) {
        return kView.error();
    }
    const FsView&              flat  = kView.value();
    const base::Result<FsNode> kNode = flat.ops->lookup(flat.self, "console");
    if (!kNode.ok()) {
        return kNode.error();
    }
    flat.ops->retain(flat.self, kNode.value());
    const base::Shared<OpenFile> kStream{new OpenFile(flat.ops, flat.self, kNode.value())};
    if (!kStream.holds()) {
        flat.ops->release(flat.self, kNode.value());
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

unsigned long FileWorld::mounts_text(char* out, unsigned long capacity) const {
    return vfs_.mounts_text(out, capacity);
}

}  // namespace cinux::fs
