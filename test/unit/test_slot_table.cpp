#include <memory>
#include <type_traits>

#include "cinux/container/slot_table.hpp"
#include "framework.hpp"
#include "test_assert.hpp"

namespace {
using cinux::base::container::SlotTable;

constexpr bool kConstantSlots = [] {
    SlotTable<unsigned int, 3> table;
    if (table.insert(10) != 0 || table.insert(20) != 1 || !table.erase(0)) {
        return false;
    }
    return *table.get(1) == 20 && table.insert(30) == 0 && *table.get(0) == 30;
}();
static_assert(kConstantSlots);
static_assert(std::is_trivially_destructible_v<SlotTable<const void*, 64>>);
constinit SlotTable<unsigned int, 2> g_constant_slots;

TEST("slot table: erase preserves other indices and pointers and reuses the lowest hole") {
    SlotTable<unsigned int, 3> table;
    ASSERT_EQ(table.insert(10), 0UL);
    ASSERT_EQ(table.insert(20), 1UL);
    auto* const kResident = table.get(1);
    ASSERT_TRUE(table.erase(0));
    ASSERT_EQ(table.insert(30), 0UL);
    ASSERT_TRUE(table.get(1) == kResident);
    ASSERT_EQ(*kResident, 20U);
}

TEST("slot table: null values occupy slots independently of the payload") {
    SlotTable<const void*, 1> table;
    ASSERT_EQ(table.insert(nullptr), 0UL);
    ASSERT_TRUE(table.get(0) != nullptr);
    ASSERT_TRUE(*table.get(0) == nullptr);
    ASSERT_EQ(table.insert(nullptr), 1UL);
    ASSERT_TRUE(table.erase(0));
    ASSERT_TRUE(table.get(0) == nullptr);
}

TEST("slot table: allocation can reserve initial indices") {
    SlotTable<unsigned int, 5> table;
    ASSERT_EQ(table.insert(10, 3), 3UL);
    ASSERT_EQ(table.insert(20, 3), 4UL);
    ASSERT_EQ(table.insert(30, 3), 5UL);
    ASSERT_TRUE(table.get(0) == nullptr);
    ASSERT_TRUE(table.get(2) == nullptr);
    ASSERT_EQ(table.vacant(6), 5UL);
}

TEST("slot table: invalid indices and zero capacity are refused") {
    SlotTable<unsigned int, 0> table;
    ASSERT_EQ(table.insert(10), 0UL);
    ASSERT_TRUE(table.get(0) == nullptr);
    ASSERT_FALSE(table.erase(0));
    ASSERT_EQ(g_constant_slots.vacant(), 0UL);
}

TEST("slot table: const search and visitation skip holes") {
    SlotTable<unsigned int, 3> table;
    ASSERT_EQ(table.insert(10), 0UL);
    ASSERT_EQ(table.insert(20), 1UL);
    ASSERT_TRUE(table.erase(0));
    const auto& const_table = table;
    ASSERT_EQ(const_table.find_if([](unsigned int value) { return value == 20; }), 1UL);
    ASSERT_EQ(const_table.find_if([](unsigned int value) { return value == 10; }), 3UL);
    unsigned long sum = 0;
    const_table.for_each([&sum](unsigned long index, unsigned int value) { sum += index + value; });
    ASSERT_EQ(sum, 21UL);
}

TEST("slot table: visiting can erase current slots and releases owned objects") {
    SlotTable<std::unique_ptr<unsigned int>, 2> table;
    ASSERT_EQ(table.insert(std::make_unique<unsigned int>(10)), 0UL);
    ASSERT_EQ(table.insert(std::make_unique<unsigned int>(20)), 1UL);
    unsigned int sum = 0;
    table.for_each([&table, &sum](unsigned long index, const std::unique_ptr<unsigned int>& value) {
        sum += *value;
        ASSERT_TRUE(table.erase(index));
    });
    ASSERT_EQ(sum, 30U);
    ASSERT_EQ(table.vacant(), 0UL);
    ASSERT_TRUE(table.get(1) == nullptr);
}
}  // namespace

int main() {
    return cinux::test::RunAll();
}
