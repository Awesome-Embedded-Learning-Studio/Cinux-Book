/**
 * @file kernel/test/test_apic.cpp
 * @brief Local APIC + I/O APIC driver mock tests (F4-M2)
 *
 * Uses plain RAM pages as fake 4 KB MMIO windows to verify driver logic without
 * touching real hardware.  The PIC->APIC switch (and real APIC use) is M2-3.
 *
 * I/O APIC registers are accessed indirectly (IOREGSEL/IOWIN), so the mock
 * verifies the selection + window-write pattern and the redirect/mask bit
 * math; full end-to-end redirect is exercised on real hardware in M2-3.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/apic/io_apic.hpp"
#include "kernel/drivers/apic/local_apic.hpp"

using cinux::drivers::apic::IOAPIC;
using cinux::drivers::apic::LocalAPIC;
using cinux::drivers::apic::kIoapicRegRedirectBase;
using cinux::drivers::apic::kRedirectMaskBit;
using cinux::drivers::apic::kRegEoi;
using cinux::drivers::apic::kRegErrorStatus;
using cinux::drivers::apic::kRegId;
using cinux::drivers::apic::kRegSpurious;
using cinux::drivers::apic::kRegTaskPriority;
using cinux::drivers::apic::kSvrEnable;

namespace {

/// A mock 4 KB MMIO window backed by plain RAM (volatile so writes through the
/// driver's volatile pointer are observable on re-read).
struct MockMmio {
    static constexpr size_t kWords = 256;  ///< covers offsets up to 0x3FF
    volatile uint32_t       words[kWords];

    void clear() {
        for (auto& w : words) {
            w = 0;
        }
    }
    volatile uint32_t* base() { return words; }
};

}  // namespace

// ============================================================
// Local APIC driver logic
// ============================================================
namespace test_local_apic {

void test_read_write_roundtrip() {
    MockMmio mmio;
    mmio.clear();
    LocalAPIC lapic;
    lapic.bind(mmio.base());
    lapic.write(kRegTaskPriority, 0x12345678);
    TEST_ASSERT_TRUE(lapic.read(kRegTaskPriority) == 0x12345678);
}

void test_enable_sets_svr() {
    MockMmio mmio;
    mmio.clear();
    LocalAPIC lapic;
    lapic.bind(mmio.base());
    lapic.enable(0xFF);
    TEST_ASSERT_TRUE((mmio.words[kRegSpurious / 4] & 0xFF) == 0xFF);
    TEST_ASSERT_TRUE((mmio.words[kRegSpurious / 4] & kSvrEnable) != 0);
}

void test_disable_clears_enable() {
    MockMmio mmio;
    mmio.clear();
    LocalAPIC lapic;
    lapic.bind(mmio.base());
    lapic.enable(0x20);
    lapic.disable();
    TEST_ASSERT_TRUE((mmio.words[kRegSpurious / 4] & kSvrEnable) == 0);
}

void test_eoi_writes_zero() {
    MockMmio mmio;
    mmio.clear();
    mmio.words[kRegEoi / 4] = 0xDEADBEEF;
    LocalAPIC lapic;
    lapic.bind(mmio.base());
    lapic.eoi();
    TEST_ASSERT_TRUE(mmio.words[kRegEoi / 4] == 0);
}

void test_id_decodes_bits_24_31() {
    MockMmio mmio;
    mmio.clear();
    mmio.words[kRegId / 4] = 0x05000000;  // APIC ID 5 in bits 24-31
    LocalAPIC lapic;
    lapic.bind(mmio.base());
    TEST_ASSERT_TRUE(lapic.id() == 5);
}

void test_clear_error() {
    MockMmio mmio;
    mmio.clear();
    mmio.words[kRegErrorStatus / 4] = 0xAB;
    LocalAPIC lapic;
    lapic.bind(mmio.base());
    lapic.clear_error();
    TEST_ASSERT_TRUE(mmio.words[kRegErrorStatus / 4] == 0);
}

}  // namespace test_local_apic

// ============================================================
// I/O APIC driver logic
// ============================================================
namespace test_ioapic {

void test_write_selects_reg_and_writes_window() {
    MockMmio mmio;
    mmio.clear();
    IOAPIC ioapic;
    ioapic.bind(mmio.base());
    ioapic.write(0x05, 0xAABBCCDD);
    // IOREGSEL (offset 0) = 0x05, IOWIN (offset 0x10 = words[4]) = value
    TEST_ASSERT_TRUE(mmio.words[0] == 0x05);
    TEST_ASSERT_TRUE(mmio.words[4] == 0xAABBCCDD);
}

void test_set_redirect_dest_in_high() {
    MockMmio mmio;
    mmio.clear();
    IOAPIC ioapic;
    ioapic.bind(mmio.base());
    ioapic.set_redirect(0, 0x20, 5);  // gsi 0, vector 0x20, dest APIC ID 5
    // Last write is the high word: IOREGSEL = redirect_base + 1, value = dest << 24
    TEST_ASSERT_TRUE(mmio.words[0] == kIoapicRegRedirectBase + 1);
    TEST_ASSERT_TRUE(mmio.words[4] == (5u << 24));
}

void test_mask_sets_bit16() {
    MockMmio mmio;
    mmio.clear();
    IOAPIC ioapic;
    ioapic.bind(mmio.base());
    ioapic.mask(0);
    // mask: write(reg, read(reg) | bit16); read(reg) returns words[4]=0 (cleared)
    TEST_ASSERT_TRUE(mmio.words[0] == kIoapicRegRedirectBase);
    TEST_ASSERT_TRUE(mmio.words[4] == kRedirectMaskBit);
}

void test_unmask_clears_bit16() {
    MockMmio mmio;
    mmio.clear();
    IOAPIC ioapic;
    ioapic.bind(mmio.base());
    ioapic.mask(0);    // sets bit16
    ioapic.unmask(0);  // reads bit16, clears it
    TEST_ASSERT_TRUE(mmio.words[4] == 0);
}

}  // namespace test_ioapic

extern "C" void run_apic_tests() {
    TEST_SECTION("APIC (F4-M2)");

    RUN_TEST(test_local_apic::test_read_write_roundtrip);
    RUN_TEST(test_local_apic::test_enable_sets_svr);
    RUN_TEST(test_local_apic::test_disable_clears_enable);
    RUN_TEST(test_local_apic::test_eoi_writes_zero);
    RUN_TEST(test_local_apic::test_id_decodes_bits_24_31);
    RUN_TEST(test_local_apic::test_clear_error);

    RUN_TEST(test_ioapic::test_write_selects_reg_and_writes_window);
    RUN_TEST(test_ioapic::test_set_redirect_dest_in_high);
    RUN_TEST(test_ioapic::test_mask_sets_bit16);
    RUN_TEST(test_ioapic::test_unmask_clears_bit16);

    TEST_SUMMARY();
}
