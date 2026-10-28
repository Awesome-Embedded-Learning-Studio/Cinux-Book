#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/input/input_config.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/devfs/input_node.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "test_assert.hpp"

namespace {

cinux::fs::DevFs           g_devices;
constexpr cinux::fs::FsOps kOps = cinux::fs::EraseFs<cinux::fs::DevFs>();
cinux::driver::EventStream g_keyboard;
cinux::driver::EventStream g_pointer;

cinux::base::Result<cinux::fs::FsNode> open_event0() {
    return g_devices.adopt_open(g_devices.lookup("event0").value(), "event0");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("input node: independent opens own their grab and final close releases it") {
    const auto kFirst  = open_event0();
    const auto kSecond = open_event0();
    ASSERT_TRUE(kFirst.ok());
    ASSERT_TRUE(kSecond.ok());
    ASSERT_TRUE(kFirst.value().ino >= cinux::driver::kInputInoBase);
    ASSERT_TRUE(kFirst.value().opaque != kSecond.value().opaque);

    cinux::fs::FsStat probed{};
    ASSERT_TRUE(g_devices.stat(kFirst.value(), probed).ok());
    ASSERT_EQ(probed.rdev, cinux::fs::MakeDeviceId(cinux::driver::kInputEventMajor, 64));

    ASSERT_EQ(g_devices.ioctl(kFirst.value(), cinux::driver::kEvdevGrab, 1), 0);
    ASSERT_EQ(g_devices.ioctl(kFirst.value(), cinux::driver::kEvdevGrab, 1),
              -cinux::syscall::kEbusy);
    ASSERT_EQ(g_devices.ioctl(kSecond.value(), cinux::driver::kEvdevGrab, 1),
              -cinux::syscall::kEbusy);
    ASSERT_EQ(g_devices.ioctl(kSecond.value(), cinux::driver::kEvdevGrab, 0),
              -cinux::syscall::kEinval);

    g_keyboard.publish(cinux::driver::kEvKey, 30, 1);
    cinux::driver::InputEventWire record{};
    ASSERT_TRUE(g_devices.read(kFirst.value(), 0, &record, sizeof(record)).ok());
    ASSERT_EQ(record.code, 30);
    const auto kRefused = g_devices.read(kSecond.value(), 0, &record, sizeof(record));
    ASSERT_TRUE(kRefused.error() == cinux::base::KernelError::kWouldBlock);

    g_devices.release(kSecond.value());
    g_devices.release(kFirst.value());
    ASSERT_EQ(g_devices.lookup("event0").value().ino, 2);
    const auto kAfter = open_event0();
    ASSERT_EQ(g_devices.ioctl(kAfter.value(), cinux::driver::kEvdevGrab, 1), 0);
    g_devices.release(kAfter.value());
}

TEST("input node: a duplicated descriptor shares one grab session") {
    cinux::fs::FileTable files;
    const auto           kSeated = open_event0();
    const auto           kSeat   = files.alloc(&kOps, &g_devices, kSeated.value());
    g_devices.release(kSeated.value());
    ASSERT_TRUE(kSeat.ok());

    const auto kShared = files.adopt(files.shared(kSeat.value()).value(), 3);
    ASSERT_TRUE(kShared.ok());
    ASSERT_EQ(files.shared(kShared.value()).value().get(),
              files.shared(kSeat.value()).value().get());

    const auto kNode = files.shared(kSeat.value()).value()->node;
    ASSERT_EQ(g_devices.ioctl(kNode, cinux::driver::kEvdevGrab, 1), 0);
    ASSERT_TRUE(files.free(kShared.value()).ok());
    const auto kStillHeld = open_event0();
    ASSERT_EQ(g_devices.ioctl(kStillHeld.value(), cinux::driver::kEvdevGrab, 1),
              -cinux::syscall::kEbusy);
    g_devices.release(kStillHeld.value());

    ASSERT_TRUE(files.free(kSeat.value()).ok());
    const auto kReleased = open_event0();
    ASSERT_EQ(g_devices.ioctl(kReleased.value(), cinux::driver::kEvdevGrab, 1), 0);
    g_devices.release(kReleased.value());
}

TEST("input node: event1 carries the pointer's minor in its minted identity") {
    const auto kPointer = g_devices.adopt_open(g_devices.lookup("event1").value(), "event1");
    ASSERT_TRUE(kPointer.ok());
    cinux::fs::FsStat probed{};
    ASSERT_TRUE(g_devices.stat(kPointer.value(), probed).ok());
    ASSERT_EQ(probed.rdev, cinux::fs::MakeDeviceId(cinux::driver::kInputEventMajor, 65));
    g_pointer.publish(2, 0, -3);
    cinux::driver::InputEventWire record{};
    ASSERT_TRUE(g_devices.read(kPointer.value(), 0, &record, sizeof(record)).ok());
    ASSERT_EQ(record.code, 0);
    ASSERT_EQ(record.value, -3);
    g_devices.release(kPointer.value());
}

}  // namespace

int main() {
    cinux::driver::DeviceModel::self().bind(g_devices);
    cinux::fs::RegisterInputDevices(g_keyboard, g_pointer);
    return cinux::test::RunAll();
}
