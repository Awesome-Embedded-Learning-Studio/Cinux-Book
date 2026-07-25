/**
 * @file kernel/test/test_hpet.cpp
 * @brief HPET timer tests (F5-M4)
 *
 * Runs inside QEMU.  B1 verifies the ACPI "HPET" table is discoverable and that
 * parse_hpet decodes QEMU's standard MMIO base (0xFED00000).  B2 adds the MMIO
 * driver mechanism tests: the capabilities register reports a sane counter
 * clock period, and the free-running main counter actually advances once
 * ENABLE_CNF is set (proving the counter is wired and running, not a green
 * mask over a dead device -- see F-VERIFY's mechanism-readback discipline).
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/acpi/acpi.hpp"
#include "kernel/drivers/hpet/hpet.hpp"

using cinux::drivers::HPET;
using cinux::drivers::g_hpet;
using cinux::drivers::acpi::HPETInfo;
using cinux::drivers::acpi::SDTHeader;
using cinux::drivers::acpi::find_table;
using cinux::drivers::acpi::parse_hpet;

// ============================================================
// ACPI HPET table discovery + parse (B1)
// ============================================================
namespace test_hpet_table {

void test_find_hpet_table_present() {
    // QEMU's default 'pc' machine exposes an ACPI HPET table ("HPET").
    const SDTHeader* hpet = find_table("HPET");
    TEST_ASSERT_NOT_NULL(hpet);
}

void test_found_hpet_signature_matches() {
    const SDTHeader* hpet = find_table("HPET");
    TEST_ASSERT_NOT_NULL(hpet);
    if (hpet == nullptr) {
        return;
    }
    TEST_ASSERT_TRUE(hpet->signature[0] == 'H' && hpet->signature[1] == 'P' &&
                     hpet->signature[2] == 'E' && hpet->signature[3] == 'T');
}

void test_parse_hpet_decodes_fed00000() {
    const SDTHeader* hpet = find_table("HPET");
    TEST_ASSERT_NOT_NULL(hpet);
    if (hpet == nullptr) {
        return;
    }
    HPETInfo info = parse_hpet(hpet);
    TEST_ASSERT_TRUE(info.present);
    // QEMU maps the HPET register block at the fixed ISA-style 0xFED00000.
    TEST_ASSERT_TRUE(info.address == 0xFED00000ULL);
}

void test_parse_hpet_length_too_short_is_absent() {
    // A table shorter than the 56-byte HPET header must decode as absent.
    SDTHeader stub{};
    stub.length   = sizeof(SDTHeader);  // 36 -- no HPET-specific fields
    HPETInfo info = parse_hpet(&stub);
    TEST_ASSERT_FALSE(info.present);
    TEST_ASSERT_TRUE(info.address == 0);
}

void test_parse_hpet_null_is_absent() {
    HPETInfo info = parse_hpet(nullptr);
    TEST_ASSERT_FALSE(info.present);
    TEST_ASSERT_TRUE(info.address == 0);
}

}  // namespace test_hpet_table

// ============================================================
// HPET MMIO driver mechanism (B2)
// ============================================================
namespace test_hpet_driver {

void test_init_succeeds() {
    // find ACPI table + map MMIO + enable + capture boot counter.
    TEST_ASSERT_TRUE(g_hpet.init());
    TEST_ASSERT_TRUE(g_hpet.available());
}

void test_period_is_sane() {
    // HPET period: >0 and <= 100 ns (1e8 fs) per spec.  QEMU is exactly 10 ns
    // (1e7 fs = 100 MHz).  A zero or huge period would mean we misread the
    // capabilities register -- the mechanism check that proves HPET is wired.
    TEST_ASSERT_TRUE(g_hpet.available());
    const uint64_t period = g_hpet.period_fs();
    TEST_ASSERT_TRUE(period > 0 && period <= 100'000'000ULL);
}

void test_counter_advances() {
    // The main counter only ticks once ENABLE_CNF is set.  If init forgot to
    // enable it, the counter reads frozen and this fails.  100 MHz => a handful
    // of reads always shows growth; the bound only guards against an infinite
    // loop if the hardware is unexpectedly dead.
    TEST_ASSERT_TRUE(g_hpet.available());
    const uint64_t first = g_hpet.counter();
    uint64_t       now   = first;
    for (int i = 0; i < 1000 && now <= first; ++i) {
        now = g_hpet.counter();
    }
    TEST_ASSERT_TRUE(now > first);
}

void test_monotonic_ns_nondecreasing() {
    // monotonic_ns must never go backwards; with the counter running it climbs.
    TEST_ASSERT_TRUE(g_hpet.available());
    const uint64_t a = g_hpet.monotonic_ns();
    const uint64_t b = g_hpet.monotonic_ns();
    TEST_ASSERT_TRUE(b >= a);
}

}  // namespace test_hpet_driver

// ============================================================
// Entry point
// ============================================================
extern "C" void run_hpet_tests() {
    TEST_SECTION("HPET (F5-M4)");

    RUN_TEST(test_hpet_table::test_find_hpet_table_present);
    RUN_TEST(test_hpet_table::test_found_hpet_signature_matches);
    RUN_TEST(test_hpet_table::test_parse_hpet_decodes_fed00000);
    RUN_TEST(test_hpet_table::test_parse_hpet_length_too_short_is_absent);
    RUN_TEST(test_hpet_table::test_parse_hpet_null_is_absent);

    // B2: bring up the MMIO driver (needs VMM, already up by this point in
    // main_test), then run the mechanism tests against the live hardware.
    g_hpet.init();
    RUN_TEST(test_hpet_driver::test_init_succeeds);
    RUN_TEST(test_hpet_driver::test_period_is_sane);
    RUN_TEST(test_hpet_driver::test_counter_advances);
    RUN_TEST(test_hpet_driver::test_monotonic_ns_nondecreasing);

    TEST_SUMMARY();
}
