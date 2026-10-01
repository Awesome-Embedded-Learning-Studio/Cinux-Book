#include "../framework/framework.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"

using cinux::arch::page::Entry;
using cinux::arch::page::kLarge;
using cinux::arch::page::kLargePageSize;
using cinux::arch::page::kLargePagePhys;
using cinux::arch::page::kPresent;
using cinux::arch::page::kWritable;
using cinux::arch::page::MakeLargePageEntry;
using cinux::arch::page::MakeTableEntry;

TEST("page: entry grammar static pins") {
    ASSERT_TRUE(sizeof(Entry) == 8);
    ASSERT_TRUE(cinux::arch::page::kLargePageSize == 0x200000);
    ASSERT_TRUE(MakeTableEntry(0x2007, kWritable).raw == 0x2003);
    ASSERT_TRUE(MakeLargePageEntry(0x200123, kWritable).raw == 0x200083);
}

TEST("page: entry width is pinned at eight bytes") {
    ASSERT_TRUE(sizeof(Entry) == 8);
}

TEST("page: table entries mask flag noise and add present+writable") {
    ASSERT_TRUE(MakeTableEntry(0x2000, kWritable).raw == 0x2003);
    ASSERT_TRUE(MakeTableEntry(0x2007, kWritable).raw == 0x2003);
}

TEST("page: large pages encode the identity 0-8MB set") {
    ASSERT_TRUE(MakeLargePageEntry(0x0, kWritable).raw == 0x83);
    ASSERT_TRUE(MakeLargePageEntry(0x200000, kWritable).raw == 0x200083);
    ASSERT_TRUE(MakeLargePageEntry(0x400000, kWritable).raw == 0x400083);
    ASSERT_TRUE(MakeLargePageEntry(0x600000, kWritable).raw == 0x600083);
}

TEST("page: large page addresses snap to 2MB alignment") {
    ASSERT_TRUE(MakeLargePageEntry(0x200123, kWritable).raw == 0x200083);
    ASSERT_TRUE(kLargePageSize == 0x200000);
}

TEST("page: deposit and extract are inverse over the phys field") {
    Entry const kEntry = MakeLargePageEntry(0x600000, kWritable);
    ASSERT_TRUE(kEntry.has(kPresent));
    ASSERT_TRUE(kEntry.has(kWritable));
    ASSERT_TRUE(kEntry.has(kLarge));
    ASSERT_TRUE(kEntry.extract(kLargePagePhys) == (0x600000 >> 21));
}

int main() {
    return cinux::test::RunAll();
}
