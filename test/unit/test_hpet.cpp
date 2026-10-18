#include <array>

#include "../framework/framework.hpp"
#include "kernel/driver/hpet/hpet_layout.hpp"
#include "test_assert.hpp"

namespace {

// Independent wire image of QEMU's 56-byte HPET table, including its checksum.
constexpr std::array<unsigned char, 56> kQemuTable = {
    0x48, 0x50, 0x45, 0x54, 0x38, 0x00, 0x00, 0x00, 0x01, 0xb4, 0x42, 0x4f, 0x43, 0x48,
    0x53, 0x20, 0x42, 0x58, 0x50, 0x43, 0x20, 0x20, 0x20, 0x20, 0x01, 0x00, 0x00, 0x00,
    0x42, 0x58, 0x50, 0x43, 0x01, 0x00, 0x00, 0x00, 0x01, 0xa2, 0x86, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xd0, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

}  // namespace

TEST("hpet: decode the firmware wire image at the specified GAS offsets") {
    ASSERT_TRUE(cinux::driver::hpet::BlockAddress(kQemuTable.data(), kQemuTable.size()) ==
                0xfed00000ULL);
    auto table = kQemuTable;
    // Exercise all eight address bytes; a six-byte decode must not silently alias.
    table[50]  = 0x12;
    table[51]  = 0x34;
    ASSERT_TRUE(cinux::driver::hpet::BlockAddress(table.data(), table.size()) ==
                0x34120000fed00000ULL);
}

TEST("hpet: reject truncated non-memory null and misaligned descriptions") {
    using cinux::driver::hpet::BlockAddress;
    ASSERT_TRUE(BlockAddress(kQemuTable.data(), 55) == 0);
    auto table = kQemuTable;
    table[40]  = 1;
    ASSERT_TRUE(BlockAddress(table.data(), table.size()) == 0);
    table     = kQemuTable;
    table[44] = 1;
    ASSERT_TRUE(BlockAddress(table.data(), table.size()) == 0);
    table = {};
    ASSERT_TRUE(BlockAddress(table.data(), table.size()) == 0);
}

TEST("hpet: period limits follow the hardware specification") {
    using cinux::driver::hpet::ValidPeriod;
    ASSERT_TRUE(!ValidPeriod(0));
    ASSERT_TRUE(ValidPeriod(1));
    ASSERT_TRUE(ValidPeriod(10'000'000));
    ASSERT_TRUE(ValidPeriod(100'000'000));
    ASSERT_TRUE(!ValidPeriod(100'000'001));
    ASSERT_TRUE(!ValidPeriod(0xf000ff53));
}

TEST("hpet: scaling preserves fractions and does not wrap after five hours") {
    using cinux::driver::hpet::Nanoseconds;
    ASSERT_TRUE(Nanoseconds(1, 10'000'000) == 10);
    ASSERT_TRUE(Nanoseconds(3, 1'500'001) == 4);
    // Six hours at QEMU's 100 MHz, beyond the old intermediate-product overflow.
    ASSERT_TRUE(Nanoseconds(2'160'000'000'000ULL, 10'000'000) == 21'600'000'000'000ULL);
    ASSERT_TRUE(Nanoseconds(1'234'567'890'123ULL, 69'841'279) == 86'223'800'458'521ULL);
}

int main() {
    return cinux::test::RunAll();
}
