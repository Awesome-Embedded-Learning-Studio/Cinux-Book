#include <cstdint>

#include "../framework/framework.hpp"
#include "kernel/arch/x86_64/ex_table.hpp"
#include "test_assert.hpp"

namespace {

using cinux::arch::ex_table::Entry;

}  // namespace

TEST("ex_table: the boot sort orders by faulting address") {
    Entry table[5] = {
        {.fault_from = 0x5000, .land_at = 0xA000}, {.fault_from = 0x1000, .land_at = 0xA100},
        {.fault_from = 0x4000, .land_at = 0xA200}, {.fault_from = 0x3000, .land_at = 0xA300},
        {.fault_from = 0x2000, .land_at = 0xA400},
    };
    cinux::arch::ex_table::SortEntries(table, 5);
    ASSERT_EQ(table[0].fault_from, 0x1000ULL);
    ASSERT_EQ(table[0].land_at, 0xA100ULL);
    ASSERT_EQ(table[1].fault_from, 0x2000ULL);
    ASSERT_EQ(table[4].fault_from, 0x5000ULL);
    ASSERT_EQ(table[4].land_at, 0xA000ULL);
}

TEST("ex_table: the walk answers hits and shrugs at misses") {
    Entry table[4] = {
        {.fault_from = 0x1000, .land_at = 0xB100},
        {.fault_from = 0x2000, .land_at = 0xB200},
        {.fault_from = 0x3000, .land_at = 0xB300},
        {.fault_from = 0x4000, .land_at = 0xB400},
    };
    const auto kCount = static_cast<unsigned long>(4);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, kCount, 0x1000), 0xB100ULL);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, kCount, 0x4000), 0xB400ULL);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, kCount, 0x2500), 0ULL);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, kCount, 0x0), 0ULL);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, kCount, 0xFFFF), 0ULL);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, 0, 0x2000), 0ULL);
}

TEST("ex_table: one entry is still a table") {
    Entry table[1] = {
        {.fault_from = 0x9000, .land_at = 0xB900},
    };
    cinux::arch::ex_table::SortEntries(table, 1);
    ASSERT_EQ(table[0].fault_from, 0x9000ULL);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, 1, 0x9000), 0xB900ULL);
    ASSERT_EQ(cinux::arch::ex_table::FindFixup(table, 1, 0x9001), 0ULL);
}

int main() {
    return cinux::test::RunAll();
}
