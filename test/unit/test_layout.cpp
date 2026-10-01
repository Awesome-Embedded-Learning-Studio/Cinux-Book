#include "../framework/framework.hpp"
#include "boot/layout.hpp"

using cinux::boot::kLmEntryVma;
using cinux::boot::kLowFree;
using cinux::boot::kMbrBase;
using cinux::boot::kPageTables;
using cinux::boot::kPmStackTop;
using cinux::boot::kStage2Spot;
using cinux::boot::kStage2Stack;

TEST("layout: migrated static pins") {
    ASSERT_TRUE(sizeof(unsigned short) == 2);
    ASSERT_TRUE(kLmEntryVma % 16 == 0);
    ASSERT_TRUE(kLmEntryVma >= static_cast<unsigned long>(kStage2Spot.offset));
    ASSERT_TRUE(kLmEntryVma < static_cast<unsigned long>(kStage2Spot.offset) +
                                  (static_cast<unsigned long>(kStage2Spot.sectors) * 512U));
    ASSERT_TRUE(kLmEntryVma > kPageTables.top);
}

TEST("layout: stack and sector pins") {
    ASSERT_TRUE(kPmStackTop % 4 == 0);
    ASSERT_TRUE(kPmStackTop > 0x1000);
    ASSERT_TRUE(kPmStackTop < 0x100000);
}

TEST("layout: image span pins") {
    ASSERT_TRUE(static_cast<unsigned long>(kStage2Spot.offset) +
                    (static_cast<unsigned long>(kStage2Spot.sectors) * 512U) <
                0x100000);
    ASSERT_TRUE(kPageTables.base >= kLowFree.base);
}

TEST("layout: semantic contract pins") {
    ASSERT_TRUE(kStage2Stack.top <= kLowFree.top);
    ASSERT_TRUE(kStage2Spot.offset == kMbrBase + 512);
    ASSERT_TRUE(kStage2Spot.segments == 0x0000);
    ASSERT_TRUE(kStage2Stack.segments == kStage2Spot.segments);
    ASSERT_TRUE(kLowFree.top == kMbrBase);
    ASSERT_TRUE(kPageTables.base % 0x1000 == 0);
    ASSERT_TRUE(kPageTables.top <= kStage2Stack.top);
}

int main() {
    return cinux::test::RunAll();
}
