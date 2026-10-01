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

TEST("e820: migrated static pins") {
    ASSERT_TRUE(sizeof(cinux::boot::MemoryMapEntry) == 24);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::MemoryMapEntry, base) == 0);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::MemoryMapEntry, length) == 8);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::MemoryMapEntry, type) == 16);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::MemoryMapEntry, acpi_attr) == 20);
    ASSERT_TRUE(sizeof(cinux::boot::MemoryMap) ==
                (static_cast<unsigned long>(cinux::boot::kE820MaxEntries) * 24U) + 4U);
}

TEST("e820: migrated classify pins") {
    ASSERT_TRUE(cinux::boot::ClassifyEntry(3) == cinux::boot::EntryType::kAcpiReclaimable);
    ASSERT_TRUE(cinux::boot::ClassifyEntry(4) == cinux::boot::EntryType::kAcpiNvs);
    ASSERT_TRUE(cinux::boot::ClassifyEntry(5) == cinux::boot::EntryType::kBad);
    ASSERT_TRUE(cinux::boot::ClassifyEntry(0) == cinux::boot::EntryType::kReserved);
    ASSERT_TRUE(cinux::boot::ClassifyEntry(0xFFFFFFFFU) == cinux::boot::EntryType::kReserved);
    ASSERT_TRUE(!cinux::boot::IsUsable(cinux::boot::EntryType::kAcpiReclaimable));
}

int main() {
    return RunAll();
}
