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
    ASSERT_TRUE(sizeof(cinux::boot::gdt::BootGdt) == 48);
    ASSERT_TRUE(sizeof(cinux::boot::gdt::DescriptorTablePointer) == 6);
}

TEST("gdt: long mode descriptors carry L=1 D=0") {
    auto const kCode64 = kTemplate.code64;
    ASSERT_TRUE((kCode64.flags_limit_high >> 5 & 1U) == 1U);
    ASSERT_TRUE((kCode64.flags_limit_high >> 6 & 1U) == 0U);
    ASSERT_TRUE(kCode64.flags_limit_high == 0xAF);
    ASSERT_TRUE(kCode64.access == 0x9A);
    ASSERT_TRUE(kTemplate.data64.flags_limit_high == 0x8F);
    ASSERT_TRUE(cinux::boot::gdt::kSelectorCode64 == 0x18);
    ASSERT_TRUE(cinux::boot::gdt::kSelectorData64 == 0x20);
}

TEST("gdt: migrated static pins") {
    ASSERT_TRUE(sizeof(cinux::boot::gdt::SegmentDescriptor) == 8);
    ASSERT_TRUE(cinux::boot::gdt::kTemplate.code.flags_limit_high == 0xCF);
    ASSERT_TRUE(kTemplate.code64.flags_limit_high == 0xAF);
    ASSERT_TRUE(kTemplate.data64.flags_limit_high == 0x8F);
}

TEST("gdt: long bit pairing L=1 D=0") {
    ASSERT_TRUE(((kTemplate.code64.flags_limit_high >> 5) & 1U) == 1U);
    ASSERT_TRUE(((kTemplate.code64.flags_limit_high >> 6) & 1U) == 0U);
    ASSERT_TRUE(kTemplate.code.base_low == 0);
    ASSERT_TRUE(kTemplate.code.base_mid == 0);
    ASSERT_TRUE(cinux::boot::gdt::kFlatLimitBytes == 0xFFFFFFFFUL);
}

int main() {
    return cinux::test::RunAll();
}
