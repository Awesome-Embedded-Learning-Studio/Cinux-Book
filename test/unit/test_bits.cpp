#include "../framework/framework.hpp"
#include "cinux/bit_ops/bit_ops.hpp"
#include "cinux/bit_ops/bitmask.hpp"

using cinux::base::bit::BitMask;
using cinux::base::bit::BitRange;
using cinux::base::bit::HighByte;
using cinux::base::bit::HighNibble;
using cinux::base::bit::LowByte;
using cinux::base::bit::LowNibble;
using cinux::base::bit::MaskBit;
using cinux::base::bit::NibbleAt;

TEST("bit_ops: nibble and byte slices") {
    ASSERT_TRUE(LowNibble(0xAB) == 0xB);
    ASSERT_TRUE(HighNibble(0xAB) == 0xA);
    ASSERT_TRUE(LowByte(0x1234) == 0x34);
    ASSERT_TRUE(HighByte(0x1234) == 0x12);
    ASSERT_TRUE(LowNibble(0xABCDEF0123456789ULL) == 0x9);
    ASSERT_TRUE(HighNibble(0xF) == 0x0);
    ASSERT_TRUE(NibbleAt(0xABCD, 12) == 0xA);
    ASSERT_TRUE(NibbleAt(0xABCD, 0) == 0xD);
}

namespace {
constexpr unsigned long long probe_deposit_value() {
    BitMask<unsigned long long> entry{1};
    entry.deposit(BitRange{.low = 12, .width = 40}, 0xABCDE);
    return entry.raw;
}
}  // namespace

constexpr BitMask<unsigned long long> const kProbeEntry{probe_deposit_value()};

TEST("bitmask: single-bit masks by word width") {
    ASSERT_TRUE(MaskBit<unsigned char>(0).raw == 0x1);
    ASSERT_TRUE(MaskBit<unsigned int>(31).raw == 0x80000000);
    ASSERT_TRUE(MaskBit<unsigned long long>(63).raw == 0x8000000000000000ULL);
    ASSERT_TRUE((MaskBit<unsigned int>(4) | MaskBit<unsigned int>(1)).raw == 0x12);
}

TEST("bitmask: deposit and extract are inverse over a field") {
    ASSERT_TRUE(probe_deposit_value() == 0xABCDE001);
    ASSERT_TRUE(kProbeEntry.extract(BitRange{.low = 12, .width = 40}) == 0xABCDE);
    ASSERT_TRUE(kProbeEntry.has(MaskBit<unsigned long long>(0)));
    ASSERT_TRUE(!kProbeEntry.has(MaskBit<unsigned long long>(1)));
}

int main() {
    return cinux::test::RunAll();
}
