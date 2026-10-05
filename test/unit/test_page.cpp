#include "../framework/framework.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "test_assert.hpp"

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

TEST("page: device doors cover the VBE framebuffer span") {
    auto const kDoors = cinux::arch::page::PlanDeviceDoors(0xFD000000, 0x300000);
    ASSERT_TRUE(kDoors.first_index == 3);
    ASSERT_TRUE(kDoors.count == 1);
}

TEST("page: device doors round straddling ends outward") {
    auto const kDoors = cinux::arch::page::PlanDeviceDoors(0xFFC00000, 0x800000);
    ASSERT_TRUE(kDoors.first_index == 3);
    ASSERT_TRUE(kDoors.count == 2);
}

TEST("page: zero-size device regions get zero doors") {
    auto const kDoors = cinux::arch::page::PlanDeviceDoors(0xFD000000, 0);
    ASSERT_TRUE(kDoors.first_index == 3);
    ASSERT_TRUE(kDoors.count == 0);
}

TEST("page: huge entries encode one-gigabyte frames") {
    ASSERT_TRUE(cinux::arch::page::MakeHugePageEntry(0xC0000000, kWritable).raw == 0xC0000083);
    ASSERT_TRUE(cinux::arch::page::MakeHugePageEntry(0x40000000, kWritable).raw == 0x40000083);
}

int main() {
    return cinux::test::RunAll();
}
