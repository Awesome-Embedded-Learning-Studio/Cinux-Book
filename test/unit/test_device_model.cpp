#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "test_assert.hpp"

namespace {

cinux::fs::DevFs g_stock;

cinux::base::Result<unsigned long> sink_read(void* /*context*/, void* /*sink*/,
                                             unsigned long /*count*/) {
    return 0;
}

cinux::base::Result<unsigned long> sink_write(void* /*context*/, const void* /*source*/,
                                              unsigned long count) {
    return count;
}

const cinux::fs::DeviceOps kSinkOps{
    .read      = sink_read,
    .write     = sink_write,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

}  // namespace

TEST("device model: the builtin pair joins the registry") {
    cinux::driver::DeviceModel::self().bind(g_stock);
    cinux::fs::RegisterBuiltinDevices();

    ASSERT_EQ(cinux::driver::DeviceModel::self().device_count(), 2UL);

    cinux::driver::DeviceFacts facts{};
    ASSERT_TRUE(cinux::driver::DeviceModel::self().nth_device(0, facts));
    ASSERT_TRUE(std::string_view(facts.name) == "null");
    ASSERT_EQ(facts.klass, cinux::driver::DeviceClass::kCharacter);
    ASSERT_EQ(facts.number, cinux::fs::MakeDeviceId(1, 3));
    ASSERT_TRUE(std::string_view(facts.bus) == "virtual");
    ASSERT_TRUE(std::string_view(facts.driver_name) == "mem");
}

TEST("device model: the builtin pair serves devfs reads") {
    ASSERT_TRUE(g_stock.lookup("null").ok());
    ASSERT_TRUE(g_stock.lookup("zero").ok());

    const cinux::base::Result<cinux::fs::FsNode> kNull     = g_stock.lookup("null");
    unsigned char                                buffer[4] = {1, 2, 3, 4};
    const cinux::base::Result<unsigned long>     kNullRead =
        g_stock.read(kNull.value(), 0, buffer, sizeof(buffer));
    ASSERT_TRUE(kNullRead.ok());
    ASSERT_EQ(kNullRead.value(), 0UL);

    const cinux::base::Result<cinux::fs::FsNode> kZero = g_stock.lookup("zero");
    const cinux::base::Result<unsigned long>     kZeroRead =
        g_stock.read(kZero.value(), 0, buffer, sizeof(buffer));
    ASSERT_TRUE(kZeroRead.ok());
    ASSERT_EQ(kZeroRead.value(), 4UL);
    for (const unsigned char kByte : buffer) {
        ASSERT_EQ(static_cast<unsigned>(kByte), 0U);
    }
}

TEST("device model: a character seat refuses a repeated name") {
    const cinux::base::Result<void> kRepeat = cinux::driver::DeviceModel::self().seat({
        .klass       = cinux::driver::DeviceClass::kCharacter,
        .name        = "null",
        .number      = cinux::fs::MakeDeviceId(1, 3),
        .ops         = &kSinkOps,
        .bus         = "virtual",
        .driver_name = "mem",
    });
    ASSERT_EQ(kRepeat.error(), cinux::base::KernelError::kAlreadyExists);
    ASSERT_EQ(cinux::driver::DeviceModel::self().device_count(), 2UL);
}

TEST("device model: a character seat without faces is refused") {
    const cinux::base::Result<void> kFaceless = cinux::driver::DeviceModel::self().seat({
        .klass       = cinux::driver::DeviceClass::kCharacter,
        .name        = "faceless",
        .number      = cinux::fs::MakeDeviceId(1, 9),
        .bus         = "virtual",
        .driver_name = "mem",
    });
    ASSERT_EQ(kFaceless.error(), cinux::base::KernelError::kInvalidArgument);
    ASSERT_EQ(cinux::driver::DeviceModel::self().device_count(), 2UL);
}

TEST("device model: a block seat records without a devfs node") {
    const cinux::base::Result<void> kSata = cinux::driver::DeviceModel::self().seat({
        .klass       = cinux::driver::DeviceClass::kBlock,
        .name        = "sata0",
        .number      = cinux::fs::MakeDeviceId(8, 0),
        .bus         = "pci",
        .driver_name = "ahci",
    });
    ASSERT_TRUE(kSata.ok());
    ASSERT_EQ(cinux::driver::DeviceModel::self().device_count(), 3UL);
    ASSERT_EQ(g_stock.lookup("sata0").error(), cinux::base::KernelError::kNotFound);

    cinux::driver::DeviceFacts block{};
    ASSERT_TRUE(cinux::driver::DeviceModel::self().nth_device(2, block));
    ASSERT_TRUE(std::string_view(block.name) == "sata0");
    ASSERT_EQ(block.klass, cinux::driver::DeviceClass::kBlock);
    ASSERT_EQ(block.number, cinux::fs::MakeDeviceId(8, 0));
    ASSERT_TRUE(std::string_view(block.bus) == "pci");
}

TEST("device model: a network seat records without a devfs node") {
    const cinux::base::Result<void> kLo = cinux::driver::DeviceModel::self().seat({
        .klass       = cinux::driver::DeviceClass::kNetwork,
        .name        = "lo",
        .bus         = "virtual",
        .driver_name = "loopback",
    });
    ASSERT_TRUE(kLo.ok());
    ASSERT_EQ(cinux::driver::DeviceModel::self().device_count(), 4UL);
    ASSERT_EQ(g_stock.lookup("lo").error(), cinux::base::KernelError::kNotFound);

    cinux::driver::DeviceFacts net{};
    ASSERT_TRUE(cinux::driver::DeviceModel::self().nth_device(3, net));
    ASSERT_TRUE(std::string_view(net.name) == "lo");
    ASSERT_EQ(net.klass, cinux::driver::DeviceClass::kNetwork);
    ASSERT_EQ(net.number, 0UL);
}

TEST("device model: seats refuse bad shapes") {
    const cinux::base::Result<void> kSlash = cinux::driver::DeviceModel::self().seat({
        .klass       = cinux::driver::DeviceClass::kNetwork,
        .name        = "bad/name",
        .bus         = "virtual",
        .driver_name = "x",
    });
    ASSERT_EQ(kSlash.error(), cinux::base::KernelError::kInvalidArgument);
    const cinux::base::Result<void> kLongBus = cinux::driver::DeviceModel::self().seat({
        .klass       = cinux::driver::DeviceClass::kNetwork,
        .name        = "ok0",
        .bus         = "a-very-long-bus",
        .driver_name = "x",
    });
    ASSERT_EQ(kLongBus.error(), cinux::base::KernelError::kInvalidArgument);
    ASSERT_EQ(cinux::driver::DeviceModel::self().device_count(), 4UL);
}

TEST("device model: the registry fills and answers past the end") {
    for (unsigned long index = 0; index < 12; ++index) {
        char name[8] = {};
        name[0]      = 'd';
        name[1]      = static_cast<char>('0' + (index / 10U));
        name[2]      = static_cast<char>('0' + (index % 10U));
        ASSERT_TRUE(cinux::driver::DeviceModel::self()
                        .seat({.klass       = cinux::driver::DeviceClass::kNetwork,
                               .name        = name,
                               .bus         = "virtual",
                               .driver_name = "x"})
                        .ok());
    }
    ASSERT_EQ(cinux::driver::DeviceModel::self().device_count(), 16UL);

    const cinux::base::Result<void> kFull = cinux::driver::DeviceModel::self().seat({
        .klass       = cinux::driver::DeviceClass::kNetwork,
        .name        = "d99",
        .bus         = "virtual",
        .driver_name = "x",
    });
    ASSERT_EQ(kFull.error(), cinux::base::KernelError::kOutOfMemory);

    cinux::driver::DeviceFacts facts{};
    ASSERT_FALSE(cinux::driver::DeviceModel::self().nth_device(16, facts));
    ASSERT_TRUE(cinux::driver::DeviceModel::self().nth_device(15, facts));
}

int main() {
    return cinux::test::RunAll();
}
