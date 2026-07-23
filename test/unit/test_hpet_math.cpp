/**
 * @file test/unit/test_hpet_math.cpp
 * @brief Unit tests for HPET ticks_to_ns (F5-M4 B2).
 *
 * ticks_to_ns is the pure arithmetic of converting counter ticks (at a given
 * femtosecond period) to nanoseconds, written overflow-safe via a split.  These
 * host tests pin both correctness at known QEMU rates and the overflow-safety
 * that the naive `ticks * period / 1e6` form would lose at year-scale uptimes.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include <stdint.h>

#include "kernel/drivers/hpet/hpet.hpp"
#include "test_framework.h"

using cinux::drivers::ticks_to_ns;

TEST("hpet_math: zero ticks is zero ns") {
    ASSERT_EQ(ticks_to_ns(10'000'000ULL, 0ULL), 0ULL);
}

TEST("hpet_math: QEMU 100 MHz period (10 ns/tick)") {
    // QEMU's HPET period is 10 ns = 1e7 fs, so ns == ticks * 10.
    ASSERT_EQ(ticks_to_ns(10'000'000ULL, 1ULL), 10ULL);
    ASSERT_EQ(ticks_to_ns(10'000'000ULL, 7ULL), 70ULL);
    ASSERT_EQ(ticks_to_ns(10'000'000ULL, 100ULL), 1000ULL);
}

TEST("hpet_math: 1 ns/tick period is identity") {
    // 1 ns = 1e6 fs: ns == ticks.
    ASSERT_EQ(ticks_to_ns(1'000'000ULL, 12'345ULL), 12'345ULL);
}

TEST("hpet_math: 100 MHz one-year uptime does not overflow") {
    // ~3.15e15 ticks * 1e7 fs would overflow uint64 in the naive form; the split
    // must still yield the exact second count.  Expected ns = seconds * 1e9.
    const uint64_t ticks_per_sec = 100'000'000ULL;  // 100 MHz
    const uint64_t secs          = 365ULL * 86400ULL;
    const uint64_t ticks         = ticks_per_sec * secs;
    const uint64_t expected_ns   = secs * 1'000'000'000ULL;
    ASSERT_EQ(ticks_to_ns(10'000'000ULL, ticks), expected_ns);
}

TEST("hpet_math: sub-microsecond remainder is exact") {
    // Ticks that do not fill a whole 1e6 block exercise the lo * period term.
    // 100 MHz: 1 tick = 10 ns, so 123456 ticks = 1_234_560 ns.
    ASSERT_EQ(ticks_to_ns(10'000'000ULL, 123'456ULL), 1'234'560ULL);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
