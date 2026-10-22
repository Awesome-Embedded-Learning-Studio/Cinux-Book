/**
 * @file    device.cpp
 * @brief   The device model's one registry, checked, seated, and read.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/device/device.hpp"

#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "cinux/text.hpp"
#include "kernel/driver/device/device_config.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/fs_config.hpp"

namespace cinux::driver {

namespace {

bool same_name(const char (&seated)[fs::kFsNameMax], std::string_view name) {
    return std::string_view(seated) == name;
}

}  // namespace

void DeviceModel::bind(fs::DevFs& devfs) {
    devfs_ = &devfs;
}

fs::DevFs& DeviceModel::bound_devfs() const {
    base::safety::Check(devfs_ != nullptr, "device model read its devfs before one was bound");
    return *devfs_;
}

base::Result<void> DeviceModel::seat(const DeviceSeat& ask) {
    if (ask.klass == DeviceClass::kCharacter && ask.ops == nullptr) {
        return base::KernelError::kInvalidArgument;
    }
    const base::Result<void> kChecked = check_seat(ask);
    if (!kChecked.ok()) {
        return kChecked;
    }
    if (ask.klass == DeviceClass::kCharacter) {
        base::safety::Check(devfs_ != nullptr,
                            "device model seated a character device before devfs was bound");
        const base::Result<void> kSeated = devfs_->register_device(ask.name, ask.number, *ask.ops);
        if (!kSeated.ok()) {
            return kSeated;
        }
    }
    return seat_record(ask);
}

base::Result<void> DeviceModel::check_seat(const DeviceSeat& ask) const {
    if (ask.name.empty() || ask.name.size() >= fs::kFsNameMax || ask.name.contains('/') ||
        ask.bus.size() >= kDeviceLabelMax || ask.driver_name.size() >= kDeviceLabelMax) {
        return base::KernelError::kInvalidArgument;
    }
    for (unsigned long index = 0; index < count_; ++index) {
        if (same_name(records_[index].name, ask.name)) {
            return base::KernelError::kAlreadyExists;
        }
    }
    if (count_ == kDeviceModelMax) {
        return base::KernelError::kOutOfMemory;
    }
    return {};
}

base::Result<void> DeviceModel::seat_record(const DeviceSeat& ask) {
    DeviceFacts& seat = records_[count_];
    seat              = DeviceFacts{};
    seat.klass        = ask.klass;
    seat.number       = ask.number;
    base::CopyText(seat.name, ask.name);
    base::CopyText(seat.bus, ask.bus);
    base::CopyText(seat.driver_name, ask.driver_name);
    ++count_;
    return {};
}

bool DeviceModel::nth_device(unsigned long ordinal, DeviceFacts& into) const {
    if (ordinal >= count_) {
        return false;
    }
    into = records_[ordinal];
    return true;
}

unsigned long DeviceModel::device_count() const {
    return count_;
}

}  // namespace cinux::driver
