/**
 * @file test/unit/test_extable.cpp
 * @brief Host-side unit tests for the exception-table search/sort primitives
 *
 * Exercises cinux::arch::extable_search / extable_sort directly with local
 * arrays -- no QEMU, no #PF. These are the pure, host-testable halves of the
 * F-EXTABLE machinery; the kernel wrappers (search_exception_tables /
 * sort_extable) just supply the linker-symbol bounds (__start/stop___ex_table).
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "kernel/arch/x86_64/extable.hpp"
#include "test_framework.h"

using cinux::arch::ExceptionTableEntry;
using cinux::arch::extable_search;
using cinux::arch::extable_sort;

TEST("extable: sort orders entries by fault_rip") {
    ExceptionTableEntry t[] = {
        {300ULL, 0ULL}, {100ULL, 0ULL}, {200ULL, 0ULL}, {500ULL, 0ULL}, {400ULL, 0ULL}};
    extable_sort(t, t + 5);
    ASSERT_EQ(t[0].fault_rip, 100ULL);
    ASSERT_EQ(t[1].fault_rip, 200ULL);
    ASSERT_EQ(t[2].fault_rip, 300ULL);
    ASSERT_EQ(t[3].fault_rip, 400ULL);
    ASSERT_EQ(t[4].fault_rip, 500ULL);
}

TEST("extable: sort leaves already-sorted range stable") {
    ExceptionTableEntry t[] = {{10ULL, 1ULL}, {20ULL, 2ULL}, {30ULL, 3ULL}};
    extable_sort(t, t + 3);
    ASSERT_EQ(t[0].fault_rip, 10ULL);
    ASSERT_EQ(t[1].fixup_rip, 2ULL);  // value carried with its key
    ASSERT_EQ(t[2].fault_rip, 30ULL);
}

TEST("extable: sort handles single and empty ranges without crashing") {
    ExceptionTableEntry one[] = {{7ULL, 70ULL}};
    extable_sort(one, one + 1);
    ASSERT_EQ(one[0].fault_rip, 7ULL);

    ExceptionTableEntry empty[1];
    extable_sort(empty, empty);  // zero-length: must be a no-op
    ASSERT_TRUE(true);
}

TEST("extable: search hits head, middle, tail of sorted range") {
    ExceptionTableEntry t[] = {{100ULL, 1000ULL},
                               {200ULL, 2000ULL},
                               {300ULL, 3000ULL},
                               {400ULL, 4000ULL},
                               {500ULL, 5000ULL}};
    const auto*         h   = extable_search(t, t + 5, 100);
    ASSERT_NOT_NULL(h);
    ASSERT_EQ(h->fixup_rip, 1000ULL);
    const auto* m = extable_search(t, t + 5, 300);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->fixup_rip, 3000ULL);
    const auto* e = extable_search(t, t + 5, 500);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(e->fixup_rip, 5000ULL);
}

TEST("extable: search misses absent fault_rip (gap / below / above)") {
    ExceptionTableEntry t[] = {{100ULL, 1ULL}, {200ULL, 2ULL}, {300ULL, 3ULL}};
    ASSERT_NULL(extable_search(t, t + 3, 150));  // between entries
    ASSERT_NULL(extable_search(t, t + 3, 50));   // below the first
    ASSERT_NULL(extable_search(t, t + 3, 999));  // above the last
}

TEST("extable: search empty range returns nullptr") {
    ExceptionTableEntry t[1];
    ASSERT_NULL(extable_search(t, t, 1234));
}

TEST("extable: search single-element range hit and miss") {
    ExceptionTableEntry t[] = {{42ULL, 9000ULL}};
    ASSERT_NOT_NULL(extable_search(t, t + 1, 42));
    ASSERT_NULL(extable_search(t, t + 1, 43));
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
