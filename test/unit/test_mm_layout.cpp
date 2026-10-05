#include "../framework/framework.hpp"
#include "kernel/mm/layout.hpp"
#include "test_assert.hpp"

using cinux::mm::DirectMapPhys;
using cinux::mm::DirectMapVirt;
using cinux::mm::IoremapVirt;
using cinux::mm::IsDirectMap;
using cinux::mm::IsKernelHalf;
using cinux::mm::kDirectMapBase;
using cinux::mm::kHeapBase;
using cinux::mm::kIoremapBase;
using cinux::mm::kKernelImageBase;
using cinux::mm::kModuleBase;
using cinux::mm::kPml4EntrySpan;
using cinux::mm::kUserBrkMmapSplit;
using cinux::mm::kUserMmapTop;
using cinux::mm::kUserNullTop;
using cinux::mm::kUserStackTop;
using cinux::mm::kUserTop;
using cinux::mm::kVmallocBase;

TEST("mm layout: kernel regions are disjoint and ordered") {
    ASSERT_TRUE(kDirectMapBase < kHeapBase);
    ASSERT_TRUE(kHeapBase < kVmallocBase);
    ASSERT_TRUE(kVmallocBase < kIoremapBase);
    ASSERT_TRUE(kIoremapBase + kPml4EntrySpan <= kKernelImageBase);
    ASSERT_TRUE(kKernelImageBase < kModuleBase);
}

TEST("mm layout: direct map conversion round trips") {
    ASSERT_TRUE(DirectMapVirt(0) == kDirectMapBase);
    ASSERT_TRUE(DirectMapVirt(0x200000) == kDirectMapBase + 0x200000);
    ASSERT_TRUE(DirectMapPhys(DirectMapVirt(0x123456)) == 0x123456);
    ASSERT_TRUE(IsDirectMap(kDirectMapBase));
    ASSERT_TRUE(IsDirectMap(kHeapBase - 1));
    ASSERT_TRUE(!IsDirectMap(kHeapBase));
    ASSERT_TRUE(!IsDirectMap(0));
}

TEST("mm layout: half classification") {
    ASSERT_TRUE(IsKernelHalf(kDirectMapBase));
    ASSERT_TRUE(IsKernelHalf(kIoremapBase));
    ASSERT_TRUE(IsKernelHalf(kModuleBase));
    ASSERT_TRUE(!IsKernelHalf(kUserTop - 1));
    ASSERT_TRUE(!IsKernelHalf(0));
}

TEST("mm layout: ioremap phase one mirrors physical offsets") {
    ASSERT_TRUE(IoremapVirt(0xFD000000) == kIoremapBase + 0xFD000000);
    ASSERT_TRUE(IoremapVirt(0) == kIoremapBase);
    ASSERT_TRUE(!IsDirectMap(IoremapVirt(0xFD000000)));
}

TEST("mm layout: image and module windows sit in the last entry") {
    ASSERT_TRUE(kKernelImageBase == 0xFFFFFFFF80000000UL);
    ASSERT_TRUE(kModuleBase == 0xFFFFFFFFC0000000UL);
    ASSERT_TRUE(kModuleBase - kKernelImageBase == 0x40000000UL);
    ASSERT_TRUE(kKernelImageBase + 0x200000 == 0xFFFFFFFF80200000UL);
}

TEST("mm layout: user half conventions") {
    ASSERT_TRUE(kUserTop == 0x0000800000000000UL);
    ASSERT_TRUE(kUserNullTop == 0x10000UL);
    ASSERT_TRUE(kUserBrkMmapSplit == 0x0000400000000000UL);
    ASSERT_TRUE(kUserMmapTop == 0x00007F8000000000UL);
    ASSERT_TRUE(kUserStackTop == 0x00007FFFFFFFF000UL);
    ASSERT_TRUE(kUserStackTop % 0x1000 == 0);
}

int main() {
    return cinux::test::RunAll();
}
