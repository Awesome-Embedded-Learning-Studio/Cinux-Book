#include <array>
#include <span>
#include <string_view>

#include "kernel/arch/x86_64/firmware_config.hpp"
#include "test/framework/framework.hpp"
#include "test/mock/firmware_config.hpp"
#include "test_assert.hpp"

TEST("firmware config: named files use big-endian directory sizes and exact names") {
    cinux::test::MockFirmwareConfig device;
    device.add_file("opt/cinux/init-env-extra", "wrong");
    device.add_file("opt/cinux/init-env", "CINUX_SESSION=gui");
    std::array<char, 128> output{};
    ASSERT_TRUE(cinux::arch::ReadFirmwareString(device.transport(), "opt/cinux/init-env", output));
    ASSERT_TRUE(std::string_view{output.data()} == "CINUX_SESSION=gui");
}

TEST("firmware config: absent firmware and missing files leave no environment") {
    std::array<char, 32> output{'x'};
    {
        cinux::test::MockFirmwareConfig device;
        device.set_item(0, {0, 0, 0, 0});
        ASSERT_FALSE(
            cinux::arch::ReadFirmwareString(device.transport(), "opt/cinux/init-env", output));
        ASSERT_EQ(output[0], '\0');
    }
    {
        cinux::test::MockFirmwareConfig device;
        device.add_file("other", "value");
        output[0] = 'x';
        ASSERT_FALSE(cinux::arch::ReadFirmwareString(device.transport(), "missing", output));
        ASSERT_EQ(output[0], '\0');
    }
}

TEST("firmware config: oversized data and empty storage cannot overrun the destination") {
    cinux::test::MockFirmwareConfig device;
    device.add_file("value", "12345678");
    std::array<char, 9> output{};
    output.back() = 'Z';
    ASSERT_FALSE(cinux::arch::ReadFirmwareString(device.transport(), "value",
                                                 std::span<char>{output}.first(8)));
    ASSERT_EQ(output[0], '\0');
    ASSERT_EQ(output.back(), 'Z');
    ASSERT_FALSE(cinux::arch::ReadFirmwareString(device.transport(), "value", {}));
    ASSERT_TRUE(cinux::arch::ReadFirmwareString(device.transport(), "value", output));
    ASSERT_EQ(output.back(), '\0');
}

TEST("firmware config: embedded NUL data and corrupt directory counts are rejected") {
    cinux::test::MockFirmwareConfig device;
    device.add_file("value", std::string_view{"a\0b", 3});
    std::array<char, 32> output{};
    ASSERT_FALSE(cinux::arch::ReadFirmwareString(device.transport(), "value", output));
    ASSERT_EQ(output[0], '\0');
    device.set_item(0x19, {0xff, 0xff, 0xff, 0xff});
    ASSERT_FALSE(cinux::arch::ReadFirmwareString(device.transport(), "value", output));
}

TEST("firmware config: an optional trailing NUL is accepted") {
    cinux::test::MockFirmwareConfig device;
    device.add_file("value", std::string_view{"ok\0", 3});
    std::array<char, 8> output{};
    ASSERT_TRUE(cinux::arch::ReadFirmwareString(device.transport(), "value", output));
    ASSERT_TRUE(std::string_view{output.data()} == "ok");
}

int main() {
    return cinux::test::RunAll();
}
