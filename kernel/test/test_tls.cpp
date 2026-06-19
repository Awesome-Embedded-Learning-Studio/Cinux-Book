/**
 * @file kernel/test/test_tls.cpp
 * @brief QEMU in-kernel tests for the TLS base helpers (F3-M2 batch 1)
 *
 * Verifies set_tls_base()/get_tls_base() round-trip over the FS base MSR.
 * The kernel never uses %fs-relative addressing (per-CPU data is via %gs),
 * so programming MSR_FS_BASE between set/get is safe even if a timer IRQ
 * fires mid-test.  Each test restores the original base on exit so the rest
 * of the suite is unaffected.
 *
 * NOTE: x86-64 requires FS/GS base MSRs to hold a CANONICAL address (bits
 * 48..63 must sign-extend bit 47); a non-canonical value raises #GP on the
 * wrmsr.  The test values below are all canonical (real TLS bases are user
 * pointers, always canonical, so clone(CLONE_SETTLS) is unaffected).
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/tls.hpp"

using cinux::arch::get_tls_base;
using cinux::arch::set_tls_base;

namespace test_tls {

void test_set_get_roundtrip() {
    uint64_t saved = get_tls_base();

    set_tls_base(0);
    TEST_ASSERT_EQ(get_tls_base(), 0u);

    // Canonical low-half value whose high 32 bits are non-zero: exercises
    // both the eax and edx halves of the MSR.
    set_tls_base(0x0000123456789ABCull);
    TEST_ASSERT_EQ(get_tls_base(), 0x0000123456789ABCull);

    set_tls_base(0);
    TEST_ASSERT_EQ(get_tls_base(), 0u);

    set_tls_base(saved);
}

void test_high_bits_preserved() {
    uint64_t saved = get_tls_base();

    // Distinct canonical values whose edx (high 32) halves differ; verifies
    // rdmsr returns the full 64-bit base, not just eax.
    set_tls_base(0x0000000012345678ull);
    TEST_ASSERT_EQ(get_tls_base(), 0x0000000012345678ull);

    set_tls_base(0x00007F0000000000ull);
    TEST_ASSERT_EQ(get_tls_base(), 0x00007F0000000000ull);

    set_tls_base(saved);
}

}  // namespace test_tls

extern "C" void run_tls_tests() {
    TEST_SECTION("TLS Tests (F3-M2-1)");

    RUN_TEST(test_tls::test_set_get_roundtrip);
    RUN_TEST(test_tls::test_high_bits_preserved);

    TEST_SUMMARY();
}
