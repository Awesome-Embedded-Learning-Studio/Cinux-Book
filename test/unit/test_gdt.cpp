#include "../framework/framework.hpp"
#include "boot/gdt/gdt.hpp"
#include "cinux/bit_ops/bit_ops.hpp"

using cinux::base::bit::HighNibble;
using cinux::base::bit::LowNibble;
using cinux::boot::gdt::kTemplate;

TEST("gdt: flat descriptors decode bit by bit") {
    auto const kCode = kTemplate.code;
    ASSERT_TRUE(HighNibble(kCode.access) == 0x9);
    ASSERT_TRUE(LowNibble(kCode.access) == 0xA);
    ASSERT_TRUE(LowNibble(kTemplate.data.access) == 0x2);
    ASSERT_TRUE((kCode.flags_limit_high >> 4) == 0xC);
    ASSERT_TRUE((kCode.flags_limit_high & 0xF) == 0xF);
}

TEST("gdt: flat model covers 4GB from zero") {
    ASSERT_TRUE(kTemplate.code.base_low == 0);
    ASSERT_TRUE(kTemplate.data.base_low == 0);
    ASSERT_TRUE(cinux::boot::gdt::kFlatLimitBytes == 0xFFFFFFFFUL);
}

TEST("gdt: packed sizes survive the host world") {
    ASSERT_TRUE(sizeof(cinux::boot::gdt::SegmentDescriptor) == 8);
    ASSERT_TRUE(sizeof(cinux::boot::gdt::BootGdt) == 24);
    ASSERT_TRUE(sizeof(cinux::boot::gdt::DescriptorTablePointer) == 6);
}

int main() {
    return cinux::test::RunAll();
}
