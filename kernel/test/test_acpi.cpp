/**
 * @file kernel/test/test_acpi.cpp
 * @brief ACPI discovery and checksum tests (F4-M1)
 *
 * Runs inside QEMU.  The checksum tests are pure logic over injected buffers;
 * the RSDP tests verify that find_rsdp() locates QEMU's standard ACPI table in
 * the EBDA or BIOS ROM area, and that the decoded fields (revision, XSDT
 * address) are sane for the M1-2 table walker.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/acpi/acpi.hpp"

using cinux::drivers::acpi::ACPIInfo;
using cinux::drivers::acpi::RSDP;
using cinux::drivers::acpi::SDTHeader;
using cinux::drivers::acpi::find_rsdp;
using cinux::drivers::acpi::find_table;
using cinux::drivers::acpi::parse_madt;
using cinux::drivers::acpi::validate_checksum;

// ============================================================
// Checksum validation (pure logic, injected buffers)
// ============================================================
namespace test_acpi_checksum {

void test_all_zero_is_valid() {
    uint8_t buf[4] = {0, 0, 0, 0};
    TEST_ASSERT_TRUE(validate_checksum(buf, 4));
}

void test_tampered_is_invalid() {
    uint8_t buf[4] = {1, 0, 0, 0};
    TEST_ASSERT_FALSE(validate_checksum(buf, 4));
}

void test_balanced_is_valid() {
    // 1 + 2 + 3 + 250 = 256 == 0 mod 256.
    uint8_t buf[4] = {1, 2, 3, 250};
    TEST_ASSERT_TRUE(validate_checksum(buf, 4));
}

void test_wrap_around_mod256() {
    // 200 + 100 = 300 == 44 mod 256, not zero.
    uint8_t unbalanced[2] = {200, 100};
    TEST_ASSERT_FALSE(validate_checksum(unbalanced, 2));

    // Adding 212 makes 512 == 0 mod 256.
    uint8_t balanced[3] = {200, 100, 212};
    TEST_ASSERT_TRUE(validate_checksum(balanced, 3));
}

}  // namespace test_acpi_checksum

// ============================================================
// RSDP discovery against QEMU's real ACPI table
// ============================================================
namespace test_acpi_rsdp {

void test_find_rsdp_returns_valid_pointer() {
    const RSDP* rsdp = find_rsdp();
    TEST_ASSERT_NOT_NULL(rsdp);
}

void test_rsdp_signature_correct() {
    const RSDP* rsdp = find_rsdp();
    TEST_ASSERT_NOT_NULL(rsdp);
    if (rsdp == nullptr) {
        return;
    }
    constexpr char kExpected[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    for (size_t i = 0; i < 8; ++i) {
        TEST_ASSERT_TRUE(rsdp->signature[i] == kExpected[i]);
    }
}

void test_rsdp_revision_valid() {
    // Revision 0 = ACPI 1.0 (RSDT only, 32-bit), >= 2 = ACPI 2.0+ (XSDT, 64-bit).
    // QEMU's default 'pc' machine exposes an ACPI 1.0 RSDP (revision 0) with a
    // 32-bit RSDT and no XSDT, so the M1-2 walker must handle both paths --
    // do not assume revision >= 2 here.
    const RSDP* rsdp = find_rsdp();
    TEST_ASSERT_NOT_NULL(rsdp);
    if (rsdp == nullptr) {
        return;
    }
    TEST_ASSERT_TRUE(rsdp->revision == 0 || rsdp->revision >= 2);
}

void test_rsdp_rsdt_address_present() {
    // rsdt_address (32-bit) exists in every RSDP revision; it is the only
    // table pointer for ACPI 1.0 and a valid pointer for 2.0+.
    const RSDP* rsdp = find_rsdp();
    TEST_ASSERT_NOT_NULL(rsdp);
    if (rsdp == nullptr) {
        return;
    }
    TEST_ASSERT_TRUE(rsdp->rsdt_address != 0);
}

void test_rsdp_xsdt_address_when_acpi2() {
    // The XSDT address is only meaningful for ACPI 2.0+.  For an ACPI 1.0 RSDP
    // (QEMU default) the field is reserved, so assert only when revision >= 2.
    const RSDP* rsdp = find_rsdp();
    TEST_ASSERT_NOT_NULL(rsdp);
    if (rsdp == nullptr) {
        return;
    }
    if (rsdp->revision >= 2) {
        TEST_ASSERT_TRUE(rsdp->xsdt_address != 0);
    }
}

}  // namespace test_acpi_rsdp

// ============================================================
// find_table signature lookup (M1-2)
// ============================================================
namespace test_acpi_find_table {

void test_find_apic_finds_madt() {
    // MADT (Multiple APIC Description Table) is signed "APIC"; M1-3 parses it.
    const SDTHeader* madt = find_table("APIC");
    TEST_ASSERT_NOT_NULL(madt);
}

void test_find_facp_finds_fadt() {
    const SDTHeader* fadt = find_table("FACP");
    TEST_ASSERT_NOT_NULL(fadt);
}

void test_found_madt_signature_matches() {
    const SDTHeader* madt = find_table("APIC");
    TEST_ASSERT_NOT_NULL(madt);
    if (madt == nullptr) {
        return;
    }
    TEST_ASSERT_TRUE(madt->signature[0] == 'A' && madt->signature[1] == 'P' &&
                     madt->signature[2] == 'I' && madt->signature[3] == 'C');
}

void test_found_madt_length_sane() {
    const SDTHeader* madt = find_table("APIC");
    TEST_ASSERT_NOT_NULL(madt);
    if (madt == nullptr) {
        return;
    }
    // MADT = header(36) + local_apic_address(4) + flags(4) + ICS entries.
    TEST_ASSERT_TRUE(madt->length >= sizeof(SDTHeader) + 8);
}

void test_find_unknown_returns_null() {
    const SDTHeader* t = find_table("ZZZZ");
    TEST_ASSERT_NULL(t);
}

}  // namespace test_acpi_find_table

// ============================================================
// MADT parsing (M1-3)
// ============================================================
namespace test_acpi_madt {

void test_parse_madt_finds_cpus() {
    const SDTHeader* madt = find_table("APIC");
    TEST_ASSERT_NOT_NULL(madt);
    if (madt == nullptr) {
        return;
    }
    ACPIInfo info = parse_madt(madt);
    // QEMU default -smp is 1, so at least the BSP is present.
    TEST_ASSERT_TRUE(info.cpu_count >= 1);
}

void test_parse_madt_lapic_address() {
    const SDTHeader* madt = find_table("APIC");
    TEST_ASSERT_NOT_NULL(madt);
    if (madt == nullptr) {
        return;
    }
    ACPIInfo info = parse_madt(madt);
    TEST_ASSERT_TRUE(info.local_apic_address == 0xFEE00000);
}

void test_parse_madt_ioapic_present() {
    const SDTHeader* madt = find_table("APIC");
    TEST_ASSERT_NOT_NULL(madt);
    if (madt == nullptr) {
        return;
    }
    ACPIInfo info = parse_madt(madt);
    TEST_ASSERT_TRUE(info.has_ioapic);
    TEST_ASSERT_TRUE(info.ioapic_address == 0xFEC00000);
}

void test_parse_madt_pcat_compat() {
    const SDTHeader* madt = find_table("APIC");
    TEST_ASSERT_NOT_NULL(madt);
    if (madt == nullptr) {
        return;
    }
    ACPIInfo info = parse_madt(madt);
    // QEMU exposes the 8259 PIC for legacy compatibility.
    TEST_ASSERT_TRUE(info.has_pcat_compat);
}

void test_parse_madt_null_is_empty() {
    ACPIInfo info = parse_madt(nullptr);
    TEST_ASSERT_TRUE(info.cpu_count == 0);
    TEST_ASSERT_FALSE(info.has_ioapic);
}

}  // namespace test_acpi_madt

// ============================================================
// Entry point
// ============================================================
extern "C" void run_acpi_tests() {
    TEST_SECTION("ACPI (F4-M1)");

    RUN_TEST(test_acpi_checksum::test_all_zero_is_valid);
    RUN_TEST(test_acpi_checksum::test_tampered_is_invalid);
    RUN_TEST(test_acpi_checksum::test_balanced_is_valid);
    RUN_TEST(test_acpi_checksum::test_wrap_around_mod256);

    RUN_TEST(test_acpi_rsdp::test_find_rsdp_returns_valid_pointer);
    RUN_TEST(test_acpi_rsdp::test_rsdp_signature_correct);
    RUN_TEST(test_acpi_rsdp::test_rsdp_revision_valid);
    RUN_TEST(test_acpi_rsdp::test_rsdp_rsdt_address_present);
    RUN_TEST(test_acpi_rsdp::test_rsdp_xsdt_address_when_acpi2);

    RUN_TEST(test_acpi_find_table::test_find_apic_finds_madt);
    RUN_TEST(test_acpi_find_table::test_find_facp_finds_fadt);
    RUN_TEST(test_acpi_find_table::test_found_madt_signature_matches);
    RUN_TEST(test_acpi_find_table::test_found_madt_length_sane);
    RUN_TEST(test_acpi_find_table::test_find_unknown_returns_null);

    RUN_TEST(test_acpi_madt::test_parse_madt_finds_cpus);
    RUN_TEST(test_acpi_madt::test_parse_madt_lapic_address);
    RUN_TEST(test_acpi_madt::test_parse_madt_ioapic_present);
    RUN_TEST(test_acpi_madt::test_parse_madt_pcat_compat);
    RUN_TEST(test_acpi_madt::test_parse_madt_null_is_empty);

    TEST_SUMMARY();
}
