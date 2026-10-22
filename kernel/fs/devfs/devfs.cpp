#include "kernel/fs/devfs/devfs.hpp"

#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/tty/pty_config.hpp"
#include "kernel/fs/devfs/devfs_config.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::fs {

namespace {

base::Result<unsigned long> null_read(void* /*context*/, void* /*sink*/, unsigned long /*count*/) {
    return 0;
}

base::Result<unsigned long> null_write(void* /*context*/, const void* /*source*/,
                                       unsigned long count) {
    return count;
}

base::Result<unsigned long> zero_read(void* /*context*/, void* sink, unsigned long count) {
    base::SetBytes(sink, 0, count);
    return count;
}

const DeviceOps kNullOps{
    .read      = null_read,
    .write     = null_write,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

const DeviceOps kZeroOps{
    .read      = zero_read,
    .write     = null_write,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

bool same_name(const char (&seated)[kFsNameMax], std::string_view name) {
    return std::string_view(seated) == name;
}

}  // namespace

void RegisterBuiltinDevices() {
    const base::Result<void> kNull = driver::DeviceModel::self().seat({
        .klass       = driver::DeviceClass::kCharacter,
        .name        = "null",
        .number      = MakeDeviceId(1, 3),
        .ops         = &kNullOps,
        .bus         = "virtual",
        .driver_name = "mem",
    });
    base::safety::Check(kNull.ok(), "device model refused the null seat");
    const base::Result<void> kZero = driver::DeviceModel::self().seat({
        .klass       = driver::DeviceClass::kCharacter,
        .name        = "zero",
        .number      = MakeDeviceId(1, 5),
        .ops         = &kZeroOps,
        .bus         = "virtual",
        .driver_name = "mem",
    });
    base::safety::Check(kZero.ok(), "device model refused the zero seat");
}

void DevFs::set_dynamic_lookup(base::Result<FsNode> (*resolve)(std::string_view path)) {
    dynamic_ = resolve;
}

void DevFs::set_open_adopter(base::Result<FsNode> (*adopt)(FsNode node, std::string_view path)) {
    adopter_ = adopt;
}

const DeviceOps* DevFs::ops_of(FsNode node) const {
    if (node.ino >= driver::kPtyInoBase) {
        return static_cast<const DeviceOps*>(node.opaque);
    }
    return static_cast<const DeviceNode*>(node.opaque)->ops;
}

base::Result<void> DevFs::register_device(std::string_view name, unsigned long rdev,
                                          const DeviceOps& ops) {
    if (name.empty() || name.size() >= kFsNameMax || name.contains('/')) {
        return base::KernelError::kInvalidArgument;
    }
    for (unsigned long index = 0; index < count_; ++index) {
        if (same_name(devices_[index].name, name)) {
            return base::KernelError::kAlreadyExists;
        }
    }
    if (count_ == kDevfsDeviceMax) {
        return base::KernelError::kOutOfMemory;
    }
    DeviceNode& seat = devices_[count_];
    seat.rdev        = rdev;
    seat.ops         = &ops;
    base::CopyBytes(seat.name, name.data(), name.size());
    ++count_;
    return {};
}

base::Result<FsNode> DevFs::lookup(std::string_view path) {
    if (path.empty()) {
        return FsNode{
            .opaque = nullptr,
            .type   = InodeType::kDirectory,
            .ino    = 1,
            .size   = 0,
        };
    }
    const unsigned long kSlash = path.find('/');
    if (kSlash != std::string_view::npos) {
        if (dynamic_ == nullptr) {
            return base::KernelError::kNotFound;
        }
        return dynamic_(path);
    }
    for (unsigned long index = 0; index < count_; ++index) {
        if (same_name(devices_[index].name, path)) {
            return FsNode{
                .opaque = &devices_[index],
                .type   = InodeType::kCharDevice,
                .ino    = 2 + index,
                .size   = 0,
            };
        }
    }
    if (dynamic_ != nullptr) {
        return dynamic_(path);
    }
    return base::KernelError::kNotFound;
}

base::Result<FsNode> DevFs::make_file(std::string_view /*path*/) {
    return base::KernelError::kNotSupported;
}

base::Result<FsNode> DevFs::make_dir(std::string_view /*path*/) {
    return base::KernelError::kNotSupported;
}

base::Result<void> DevFs::unlink(std::string_view /*path*/) {
    return base::KernelError::kNotSupported;
}

base::Result<unsigned long> DevFs::read(FsNode node, unsigned long /*offset*/, void* sink,
                                        unsigned long count) const {
    if (node.type != InodeType::kCharDevice) {
        return base::KernelError::kIsADirectory;
    }
    const DeviceOps* const kOps = ops_of(node);
    return kOps->read(kOps->context, sink, count);
}

base::Result<unsigned long> DevFs::write(FsNode node, unsigned long /*offset*/, const void* source,
                                         unsigned long count) const {
    if (node.type != InodeType::kCharDevice) {
        return base::KernelError::kIsADirectory;
    }
    const DeviceOps* const kOps = ops_of(node);
    return kOps->write(kOps->context, source, count);
}

base::Result<bool> DevFs::read_dir(FsNode node, unsigned long index, FsDirent& entry) {
    if (node.type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }
    entry      = FsDirent{};
    entry.ino  = 1;
    entry.type = InodeType::kDirectory;
    if (index == 0) {
        entry.name[0] = '.';
        return true;
    }
    if (index == 1) {
        entry.name[0] = '.';
        entry.name[1] = '.';
        return true;
    }
    const unsigned long kSeat = index - 2;
    if (kSeat >= count_) {
        return false;
    }
    base::CopyBytes(entry.name, devices_[kSeat].name, kFsNameMax);
    entry.ino  = 2 + kSeat;
    entry.type = InodeType::kCharDevice;
    return true;
}

base::Result<void> DevFs::stat(FsNode node, FsStat& probed) {
    probed      = FsStat{};
    probed.ino  = node.ino;
    probed.mode = kFsDirMode;
    if (node.type != InodeType::kCharDevice) {
        return {};
    }
    probed.mode = kFsCharDevMode;
    if (node.ino < driver::kPtyInoBase) {
        probed.rdev = static_cast<const DeviceNode*>(node.opaque)->rdev;
        return {};
    }
    const unsigned long kSeat  = node.ino - driver::kPtyInoBase - 1UL;
    const unsigned long kIndex = kSeat / 2UL;
    const unsigned long kMajor =
        (kSeat % 2UL) == 0UL ? driver::kPtyMasterMajor : driver::kPtySlaveMajor;
    probed.rdev = MakeDeviceId(kMajor, kIndex);
    return {};
}

void DevFs::retain(FsNode node) const {
    if (node.type != InodeType::kCharDevice) {
        return;
    }
    const DeviceOps* const kOps = ops_of(node);
    if (kOps->retain != nullptr) {
        kOps->retain(kOps->context);
    }
}

void DevFs::release(FsNode node) const {
    if (node.type != InodeType::kCharDevice) {
        return;
    }
    const DeviceOps* const kOps = ops_of(node);
    if (kOps->release != nullptr) {
        kOps->release(kOps->context);
    }
}

bool DevFs::readable(FsNode node) const {
    if (node.type != InodeType::kCharDevice) {
        return true;
    }
    const DeviceOps* const kOps = ops_of(node);
    if (kOps->has_input == nullptr) {
        return true;
    }
    return kOps->has_input(kOps->context);
}

long long DevFs::ioctl(FsNode node, unsigned long long word, unsigned long long argument) const {
    if (node.type != InodeType::kCharDevice) {
        return -syscall::kEnotty;
    }
    const DeviceOps* const kOps = ops_of(node);
    if (kOps->ioctl == nullptr) {
        return -syscall::kEnotty;
    }
    return kOps->ioctl(kOps->context, word, argument);
}

base::Result<FsNode> DevFs::adopt_open(FsNode node, std::string_view path) {
    if (adopter_ == nullptr) {
        return node;
    }
    return adopter_(node, path);
}

}  // namespace cinux::fs
