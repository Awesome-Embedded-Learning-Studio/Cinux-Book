#include <cstddef>

#include "../framework/framework.hpp"
#include "boot/gdt/gdt.hpp"
#include "cinux/bit_ops/bit_ops.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "test_assert.hpp"

using cinux::base::bit::HighNibble;
using cinux::base::bit::LowNibble;
using cinux::boot::gdt::kTemplate;

constexpr cinux::arch::gdt::KernelGdt kTable = cinux::arch::gdt::MakeKernelGdt();

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

TEST("gdt: kernel table owns seven long-mode slots") {
    ASSERT_TRUE(sizeof(cinux::arch::gdt::KernelGdt) == 56);
    ASSERT_TRUE(kTable.null.access == 0);
    ASSERT_TRUE(kTable.code.access == 0x9A);
    ASSERT_TRUE(kTable.code.flags_limit_high == 0xAF);
    ASSERT_TRUE(kTable.data.access == 0x92);
    ASSERT_TRUE(kTable.data.flags_limit_high == 0x8F);
}

TEST("gdt: user half joins the table with ring-3 access") {
    ASSERT_TRUE(kTable.tss_low.access == 0);
    ASSERT_TRUE(kTable.user_data.access == 0xF2);
    ASSERT_TRUE(kTable.user_data.flags_limit_high == 0x8F);
    ASSERT_TRUE(kTable.user_code.access == 0xFA);
    ASSERT_TRUE(kTable.user_code.flags_limit_high == 0xAF);
}

TEST("gdt: user code access derives from the ring vocabulary") {
    using cinux::arch::gdt::SegmentAccess;
    auto const kExpected = static_cast<unsigned char>(
        SegmentAccess::kPresent | SegmentAccess::kRing3 | SegmentAccess::kCodeData |
        SegmentAccess::kExecutable | SegmentAccess::kReadWrite);
    ASSERT_TRUE(kExpected == kTable.user_code.access);
}

TEST("gdt: user data access derives from the ring vocabulary") {
    using cinux::arch::gdt::SegmentAccess;
    auto const kExpected =
        static_cast<unsigned char>(SegmentAccess::kPresent | SegmentAccess::kRing3 |
                                   SegmentAccess::kCodeData | SegmentAccess::kReadWrite);
    ASSERT_TRUE(kExpected == kTable.user_data.access);
}

TEST("gdt: sysret star base carries rpl 3 into both selectors") {
    ASSERT_TRUE(cinux::arch::gdt::kSysretStarBase == 0x23);
    ASSERT_TRUE(cinux::arch::gdt::kSelectorTss == 0x18);
    ASSERT_TRUE(cinux::arch::gdt::kSelectorUserData == 0x2B);
    ASSERT_TRUE(cinux::arch::gdt::kSelectorUserCode == 0x33);
}

TEST("gdt: tss descriptor splits its base across both slots") {
    auto const kPair = cinux::arch::gdt::MakeTssDescriptor(0x123456789ABCULL, 0x67U);
    ASSERT_TRUE(sizeof(cinux::arch::gdt::TssDescriptor) == 16);
    ASSERT_TRUE(kPair.low.base_low == 0x9ABC);
    ASSERT_TRUE(kPair.low.base_mid == 0x78);
    ASSERT_TRUE(kPair.low.base_high == 0x56);
    ASSERT_TRUE(kPair.base_high == 0x1234);
    ASSERT_TRUE(kPair.reserved == 0);
}

TEST("gdt: tss descriptor encodes available-64-bit access with byte limit") {
    auto const kPair = cinux::arch::gdt::MakeTssDescriptor(0x123456789ABCULL, 0x67U);
    ASSERT_TRUE(kPair.low.access == 0x89);
    ASSERT_TRUE(kPair.low.limit_low == 0x67);
    ASSERT_TRUE(kPair.low.flags_limit_high == 0);
}

TEST("gdt: the 64-bit task struct keeps its architecture offsets") {
    ASSERT_TRUE(sizeof(cinux::arch::tss::Tss) == 104);
    ASSERT_TRUE(offsetof(cinux::arch::tss::Tss, rsp0) == 0x04);
    ASSERT_TRUE(offsetof(cinux::arch::tss::Tss, ist) == 0x24);
}

TEST("gdt: kernel code slot pins L=1 D=0 granularity") {
    auto const kCode = kTable.code;
    ASSERT_TRUE(((kCode.flags_limit_high >> 5) & 1U) == 1U);
    ASSERT_TRUE(((kCode.flags_limit_high >> 6) & 1U) == 0U);
    ASSERT_TRUE(((kCode.flags_limit_high >> 7) & 1U) == 1U);
}

TEST("gdt: kernel access bytes derive from the bit vocabulary") {
    using cinux::arch::gdt::SegmentAccess;
    auto const kCode = static_cast<unsigned char>(
        SegmentAccess::kPresent | SegmentAccess::kRing0 | SegmentAccess::kCodeData |
        SegmentAccess::kExecutable | SegmentAccess::kReadWrite);
    auto const kData =
        static_cast<unsigned char>(SegmentAccess::kPresent | SegmentAccess::kRing0 |
                                   SegmentAccess::kCodeData | SegmentAccess::kReadWrite);
    ASSERT_TRUE(kCode == kTable.code.access);
    ASSERT_TRUE(kData == kTable.data.access);
}

TEST("gdt: flag nibble vocabulary lands in the template bytes") {
    using cinux::arch::gdt::SegmentFlags;
    auto const kCodeNibble =
        static_cast<unsigned char>(SegmentFlags::kGranularity4K | SegmentFlags::kLongMode);
    auto const kDataNibble = static_cast<unsigned char>(SegmentFlags::kGranularity4K);
    ASSERT_TRUE((kTable.code.flags_limit_high >> 4) == kCodeNibble);
    ASSERT_TRUE((kTable.data.flags_limit_high >> 4) == kDataNibble);
}

int main() {
    return cinux::test::RunAll();
}
