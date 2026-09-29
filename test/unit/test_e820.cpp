#include "../framework/framework.hpp"
#include "boot/e820/e820.hpp"

using cinux::boot::ClassifyEntry;
using cinux::boot::EntryType;
using cinux::test::RunAll;

TEST("e820: constexpr mapping agrees at runtime") {
    ASSERT_TRUE(ClassifyEntry(1) == EntryType::kUsable);
    ASSERT_TRUE(ClassifyEntry(0) == EntryType::kReserved);
    ASSERT_TRUE(ClassifyEntry(0xFFFFFFFFU) == EntryType::kReserved);
    ASSERT_TRUE(ClassifyEntry(5) == EntryType::kBad);
}

TEST("e820: memory map is host-constructible") {
    cinux::boot::MemoryMap map{};
    ASSERT_EQ(map.count, 0U);
    map.entries[0].base   = 0x100000;
    map.entries[0].length = 0x1000;
    map.entries[0].type   = 1;
    ASSERT_EQ(ClassifyEntry(map.entries[0].type), EntryType::kUsable);
}

int main() {
    return RunAll();
}
