/**
 * @file kernel/test/test_kallsyms.cpp
 * @brief QEMU in-kernel tests for KALLSYMS address->symbol lookup (FO batch 1a)
 *
 * Feeds a small sorted fixture table into kallsyms_set_table() and validates
 * the binary-search resolution: exact hits, intra-function offsets, the
 * before-first-symbol boundary, after-last-symbol, the no-table fallback, and
 * availability/count bookkeeping.  The production kernel feeds a real
 * nm-generated table; these tests only exercise the lookup logic.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/lib/kallsyms.hpp"

using cinux::lib::kallsyms_available;
using cinux::lib::kallsyms_count;
using cinux::lib::kallsyms_lookup;
using cinux::lib::kallsyms_set_table;
using cinux::lib::KallsymEntry;

namespace {

// Fixture table: must be ascending by addr (the build-time generator's contract).
const KallsymEntry kFixture[] = {
    {0xFFFFFFFF80100000ULL, "kmain"},
    {0xFFFFFFFF80101000ULL, "do_foo"},
    {0xFFFFFFFF80102000ULL, "do_bar"},
    {0xFFFFFFFF80103000ULL, "kexit"},
};
constexpr size_t kFixtureCount = sizeof(kFixture) / sizeof(kFixture[0]);

bool streq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

}  // namespace

// ============================================================
// Test 1: exact address hit yields bare name (offset 0)
// ============================================================

namespace test_kallsyms_exact {

void test_exact_hit() {
    char buf[64];
    bool ok = kallsyms_lookup(0xFFFFFFFF80101000ULL, buf, sizeof(buf));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(streq(buf, "do_foo"));
}

}  // namespace test_kallsyms_exact

// ============================================================
// Test 2: intra-function address yields "name+0xoffset"
// ============================================================

namespace test_kallsyms_offset {

void test_within_function() {
    char buf[64];
    bool ok = kallsyms_lookup(0xFFFFFFFF80101020ULL, buf, sizeof(buf));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(streq(buf, "do_foo+0x20"));
}

}  // namespace test_kallsyms_offset

// ============================================================
// Test 3: address before the first symbol -> false + "0xADDR"
// ============================================================

namespace test_kallsyms_before {

void test_before_first_symbol() {
    char buf[64];
    bool ok = kallsyms_lookup(0xFFFFFFFF80100000ULL - 1, buf, sizeof(buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(streq(buf, "0xffffffff800fffff"));
}

}  // namespace test_kallsyms_before

// ============================================================
// Test 4: address after the last symbol resolves to last + offset
// ============================================================

namespace test_kallsyms_after {

void test_after_last_symbol() {
    char buf[64];
    bool ok = kallsyms_lookup(0xFFFFFFFF80103100ULL, buf, sizeof(buf));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(streq(buf, "kexit+0x100"));
}

}  // namespace test_kallsyms_after

// ============================================================
// Test 5: no table registered -> false + raw "0xADDR"
// ============================================================

namespace test_kallsyms_notable {

void test_no_table_fallback() {
    kallsyms_set_table(nullptr, 0);
    TEST_ASSERT_FALSE(kallsyms_available());

    char buf[64];
    bool ok = kallsyms_lookup(0xFFFFFFFF80101000ULL, buf, sizeof(buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(streq(buf, "0xffffffff80101000"));

    // Restore the fixture so later suites (and the production panic path) keep
    // a usable table.
    kallsyms_set_table(kFixture, kFixtureCount);
    TEST_ASSERT_TRUE(kallsyms_available());
}

}  // namespace test_kallsyms_notable

// ============================================================
// Test 6: availability / count bookkeeping
// ============================================================

namespace test_kallsyms_meta {

void test_availability_and_count() {
    TEST_ASSERT_TRUE(kallsyms_available());
    TEST_ASSERT_EQ(kallsyms_count(), static_cast<size_t>(kFixtureCount));
}

}  // namespace test_kallsyms_meta

// ============================================================
// Entry point
// ============================================================

extern "C" void run_kallsyms_tests() {
    kallsyms_set_table(kFixture, kFixtureCount);  // inject the fixture table
    TEST_ASSERT_TRUE(kallsyms_available());

    TEST_SECTION("KALLSYMS Lookup Tests (FO)");

    RUN_TEST(test_kallsyms_exact::test_exact_hit);
    RUN_TEST(test_kallsyms_offset::test_within_function);
    RUN_TEST(test_kallsyms_before::test_before_first_symbol);
    RUN_TEST(test_kallsyms_after::test_after_last_symbol);
    RUN_TEST(test_kallsyms_notable::test_no_table_fallback);
    RUN_TEST(test_kallsyms_meta::test_availability_and_count);

    TEST_SUMMARY();
}
