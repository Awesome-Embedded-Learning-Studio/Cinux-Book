#include "../framework/framework.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/memory.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/mm/page_walk.hpp"
#include "test_assert.hpp"

using cinux::arch::page::Entry;
using cinux::arch::page::kPresent;
using cinux::arch::page::kWritable;
using cinux::arch::page::MakeHugePageEntry;
using cinux::arch::page::MakeLargePageEntry;
using cinux::arch::page::MakeTableEntry;
using cinux::mm::walk::IndexRange;
using cinux::mm::walk::MapPage;
using cinux::mm::walk::TranslatePage;
using cinux::mm::walk::UnmapPage;

namespace {

using cinux::base::operator""_GiB;
using cinux::base::operator""_KiB;
using cinux::base::operator""_MiB;

constexpr unsigned long kPoolBytes = 64_KiB;

struct HostWorld {
    unsigned char pool[kPoolBytes] = {};
    unsigned long cursor           = 0x1000;
    unsigned long taken            = 0;

    Entry* table(unsigned long physical) { return reinterpret_cast<Entry*>(pool + physical); }

    unsigned long take_page() {
        if (cursor + 4096 > kPoolBytes) {
            return 0;
        }
        cinux::base::SetBytes(pool + cursor, 0, 4096);
        unsigned long const kPhysical = cursor;
        cursor += 4096;
        ++taken;
        return kPhysical;
    }
};

unsigned long compose(unsigned long directory_slot, unsigned long table_slot,
                      unsigned long offset) {
    Entry address{};
    address.deposit(IndexRange(cinux::mm::walk::WalkLevel::kPml4), 5);
    address.deposit(IndexRange(cinux::mm::walk::WalkLevel::kPdpt), 7);
    address.deposit(IndexRange(cinux::mm::walk::WalkLevel::kPageDirectory), directory_slot);
    address.deposit(IndexRange(cinux::mm::walk::WalkLevel::kPageTable), table_slot);
    return address.raw | offset;
}

}  // namespace

TEST("vmm walk: map creates three tables and translates round trip") {
    HostWorld           world;
    unsigned long const kRoot = world.take_page();
    unsigned long const kSpot = compose(9, 11, 0xABC);
    ASSERT_TRUE(MapPage(world, kRoot, kSpot, 0xABC000));
    ASSERT_TRUE(world.taken == 4);
    ASSERT_TRUE(TranslatePage(world, kRoot, kSpot) == 0xABCABC);
}

TEST("vmm walk: unmapped addresses translate to zero") {
    HostWorld           world;
    unsigned long const kRoot = world.take_page();
    ASSERT_TRUE(TranslatePage(world, kRoot, compose(1, 1, 0)) == 0);
}

TEST("vmm walk: unmap reports the entry and clears the slot") {
    HostWorld           world;
    unsigned long const kRoot = world.take_page();
    unsigned long const kSpot = compose(9, 11, 0);
    ASSERT_TRUE(MapPage(world, kRoot, kSpot, 0xABC000));
    Entry const kGone = UnmapPage(world, kRoot, kSpot);
    ASSERT_TRUE(kGone.has(kPresent));
    ASSERT_TRUE(TranslatePage(world, kRoot, kSpot) == 0);
    Entry const kAgain = UnmapPage(world, kRoot, kSpot);
    ASSERT_TRUE(kAgain.raw == 0);
}

TEST("vmm walk: remapping overwrites the previous target") {
    HostWorld           world;
    unsigned long const kRoot = world.take_page();
    unsigned long const kSpot = compose(6, 8, 0x40);
    ASSERT_TRUE(MapPage(world, kRoot, kSpot, 0x111000));
    ASSERT_TRUE(MapPage(world, kRoot, kSpot, 0x222000));
    ASSERT_TRUE(TranslatePage(world, kRoot, kSpot) == 0x222040);
}

TEST("vmm walk: 2MiB and 1GiB leaves translate with offsets") {
    HostWorld           world;
    unsigned long const kRoot     = world.take_page();
    Entry* const        kPml4     = world.table(kRoot);
    unsigned long const kPdptPhys = world.take_page();
    unsigned long const kPdPhys   = world.take_page();
    Entry* const        kPdpt     = world.table(kPdptPhys);
    Entry* const        kPd       = world.table(kPdPhys);
    kPml4[0]                      = MakeTableEntry(kPdptPhys, kWritable);
    kPdpt[0]                      = MakeTableEntry(kPdPhys, kWritable);
    kPd[3]                        = MakeLargePageEntry(6_MiB, kWritable);
    kPdpt[2]                      = MakeHugePageEntry(2_GiB, kWritable);
    ASSERT_TRUE(TranslatePage(world, kRoot, 6_MiB + 0x1234) == 0x601234);
    ASSERT_TRUE(TranslatePage(world, kRoot, 2_GiB + 0x4567) == 2_GiB + 0x4567);
}

TEST("vmm walk: map refuses to split a large leaf") {
    HostWorld           world;
    unsigned long const kRoot     = world.take_page();
    Entry* const        kPml4     = world.table(kRoot);
    unsigned long const kPdptPhys = world.take_page();
    unsigned long const kPdPhys   = world.take_page();
    Entry* const        kPdpt     = world.table(kPdptPhys);
    Entry* const        kPd       = world.table(kPdPhys);
    kPml4[0]                      = MakeTableEntry(kPdptPhys, kWritable);
    kPdpt[0]                      = MakeTableEntry(kPdPhys, kWritable);
    kPd[3]                        = MakeLargePageEntry(6_MiB, kWritable);
    kPdpt[2]                      = MakeHugePageEntry(2_GiB, kWritable);
    ASSERT_TRUE(!MapPage(world, kRoot, 6_MiB + 0x1000, 0xABC000));
    ASSERT_TRUE(!MapPage(world, kRoot, 2_GiB + 0x1000, 0xABC000));
}

int main() {
    return cinux::test::RunAll();
}
