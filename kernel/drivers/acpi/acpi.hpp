/**
 * @file kernel/drivers/acpi/acpi.hpp
 * @brief ACPI static-table discovery and structures (F4-M1)
 *
 * Minimal ACPI support for SMP bring-up: locate the RSDP, then (M1-2) walk the
 * XSDT/RSDT to find the MADT, FADT and HPET tables.  There is deliberately no
 * AML interpreter -- only the static tables the kernel needs: the Local APIC
 * base address, the CPU APIC ID list, the I/O APIC base, and the IRQ source
 * overrides.
 *
 * All tables live in conventional memory (the RSDP below 1 MB, the SDTs
 * anywhere in low RAM) and are reached through the mini loader's identity
 * direct map via phys_to_virt().  Unlike the APIC MMIO region, these are
 * ordinary RAM so the cache-enabled direct map is correct here.
 *
 * Namespace: cinux::drivers::acpi
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::drivers::acpi {

/// RSDP signature, 8 bytes: "RSD PTR " (two trailing spaces).
constexpr char kRsdpSignature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};

/// ACPI 2.0 Root System Description Pointer.
///
/// The first 20 bytes are the ACPI 1.0 layout, checksummed by @ref checksum.
/// The remaining fields are ACPI 2.0+ and are covered by @ref extended_checksum
/// over @ref length bytes.
struct [[gnu::packed]] RSDP {
    char     signature[8];  ///< "RSD PTR "
    uint8_t  checksum;      ///< byte sum of [0, 20) == 0
    char     oem_id[6];
    uint8_t  revision;           ///< 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;       ///< RSDT physical address (32-bit)
    uint32_t length;             ///< total RSDP length (36 for 2.0+)
    uint64_t xsdt_address;       ///< XSDT physical address (64-bit)
    uint8_t  extended_checksum;  ///< byte sum of [0, length) == 0
    uint8_t  reserved[3];
};
static_assert(sizeof(RSDP) == 36, "ACPI 2.0 RSDP must be 36 bytes");

/// Common header of every ACPI System Description Table (MADT, FADT, HPET...).
///
/// Defined here in M1-1 even though table walking arrives in M1-2: the layout
/// is the contract every SDT shares, and the RSDP tests already need it to be
/// stable.
struct [[gnu::packed]] SDTHeader {
    char     signature[4];  ///< e.g. "APIC", "FACP", "HPET"
    uint32_t length;        ///< whole table length, header included
    uint8_t  revision;
    uint8_t  checksum;  ///< byte sum of all bytes over length == 0
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};
static_assert(sizeof(SDTHeader) == 36, "ACPI SDT header must be 36 bytes");

/// Validate an ACPI checksum: the byte sum over the first @p len bytes must be
/// 0 (mod 256).
///
/// @param table  Pointer to the table (already mapped via the direct map).
/// @param len    Number of bytes to sum.
/// @return       true if the checksum is valid.
bool validate_checksum(const void* table, size_t len);

/// Locate the RSDP.
///
/// Searches the EBDA (segment pointer at physical 0x40E, first 1 KB) then the
/// BIOS ROM area 0xE0000-0xFFFFF in 16-byte strides.  Both the ACPI 1.0
/// checksum (first 20 bytes) and, for revision >= 2, the extended checksum
/// (whole table) are verified before the pointer is returned.
///
/// @return  RSDP via the direct map, or nullptr if not found / invalid.
const RSDP* find_rsdp();

/// Find an ACPI table by its 4-byte signature (e.g. "APIC", "FACP", "HPET").
///
/// Walks the RSDT (ACPI 1.0, 32-bit entries) or XSDT (ACPI 2.0+, 64-bit
/// entries) pointed to by the RSDP, validates each candidate's checksum, and
/// returns the first table whose signature matches.  QEMU's default 'pc'
/// machine uses the RSDT path (rev 0).
///
/// @param signature  Exactly 4 bytes compared (not NUL-terminated).
/// @return  Matching table via the direct map, or nullptr if not found.
const SDTHeader* find_table(const char* signature);

// ============================================================
// MADT (Multiple APIC Description Table, signature "APIC")
// ============================================================

/// MADT flags bit 0: a PC-AT-compatible 8259 PIC is present.
constexpr uint8_t kMadtPcatCompat = 0x01;

/// MADT header: the common SDT header, the Local APIC base address and flags,
/// then a variable-length list of Interrupt Controller Structures.
struct [[gnu::packed]] MADTHeader {
    SDTHeader header;
    uint32_t  local_apic_address;  ///< Local APIC MMIO base (32-bit, < 4 GB)
    uint32_t  flags;               ///< bit 0 = PC-AT-compatible 8259 present
};

/// Common prefix of every Interrupt Controller Structure (ICS) entry.
struct [[gnu::packed]] ICSHeader {
    uint8_t type;
    uint8_t length;  ///< total entry size, including this header
};

/// ICS type codes (subset -- only the entries M1-3 consumes).
constexpr uint8_t kIcsProcessorLocalApic      = 0;
constexpr uint8_t kIcsIoapic                  = 1;
constexpr uint8_t kIcsInterruptSourceOverride = 2;

/// Processor Local APIC (ICS type 0).
struct [[gnu::packed]] ProcessorLocalAPICEntry {
    uint8_t  type;    ///< 0
    uint8_t  length;  ///< 8
    uint8_t  processor_id;
    uint8_t  apic_id;  ///< Local APIC ID (IPI target)
    uint32_t flags;    ///< bit 0 = enabled, bit 1 = online capable
};

/// I/O APIC (ICS type 1).
struct [[gnu::packed]] IOAPICEntry {
    uint8_t  type;    ///< 1
    uint8_t  length;  ///< 12
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;  ///< I/O APIC MMIO base (32-bit)
    uint32_t gsi_base;        ///< global system interrupt base
};

/// Interrupt Source Override (ICS type 2): maps an ISA IRQ to a GSI.
struct [[gnu::packed]] InterruptSourceOverrideEntry {
    uint8_t  type;        ///< 2
    uint8_t  length;      ///< 10
    uint8_t  bus;         ///< 0 = ISA
    uint8_t  source_irq;  ///< ISA IRQ 0-15 being remapped
    uint32_t global_irq;  ///< GSI it maps to
    uint16_t flags;       ///< polarity (bits 0-1) + trigger mode (bits 2-3)
};

/// Upper bound on CPUs we record from the MADT.  Distinct from the runtime CPU
/// limit (cinux::proc::kMaxCpus = 8): ACPI may report more LAPICs than the
/// kernel actually runs; ap_main caps AP bring-up at proc::kMaxCpus.  DEBT-018
/// renamed this from kMaxCpus to avoid the same-name clash across namespaces.
constexpr size_t kMaxAcpiLapics = 16;

/// Decoded ACPI information consumed by M2 (APIC init).
struct ACPIInfo {
    uint64_t local_apic_address;  ///< LAPIC MMIO base (0xFEE00000 on QEMU)
    uint64_t ioapic_address;      ///< first I/O APIC MMIO base (0xFEC00000); 0 if none
    uint32_t ioapic_gsi_base;     ///< GSI base of the first I/O APIC
    bool     has_ioapic;
    bool     has_pcat_compat;  ///< 8259 PIC present (affects IRQ0 override)

    uint8_t  cpu_apic_ids[kMaxAcpiLapics];  ///< APIC IDs of enabled CPUs
    uint32_t cpu_count;

    /// One ISA IRQ -> GSI override (QEMU typically remaps IRQ0 -> GSI2).
    struct IrqOverride {
        uint8_t  source_irq;
        uint32_t global_irq;
        uint16_t flags;
    };
    IrqOverride irq_overrides[16];
    uint32_t    irq_override_count;
};

/// Parse the MADT into ACPIInfo.
///
/// Walks the ICS entries after the MADT header, collecting enabled CPU APIC
/// IDs, the first I/O APIC base/gsi_base, the PC-AT-compat flag, and the ISA
/// IRQ source overrides.  Entries of unknown type (NMI, LAPIC NMI, x2APIC...)
/// are skipped.
///
/// @param madt  MADT via the direct map (from find_table("APIC")), or nullptr.
/// @return      Decoded info; cpu_count==0 if the table is absent/invalid.
ACPIInfo parse_madt(const SDTHeader* madt);

// ============================================================
// HPET (IA-PC High-Precision Event Timer, signature "HPET")  -- F5-M4
// ============================================================

/// HPET description table (ACPI "HPET", 56 bytes).
///
/// After the common SDT header come the Event Timer Block ID, the MMIO base as
/// an ACPI Generic Address Structure (12 bytes -- space/bitwidth/bitoffset/
/// accesssize then the 8-byte physical address), then the sequence number,
/// minimum tick and page-protection hints.  Only @ref base_address is consumed
/// by the F5-M4 driver (the period and counter live in MMIO registers, not in
/// this table); the other fields are documented for the periodic-IRQ follow-up.
struct [[gnu::packed]] HPETHeader {
    SDTHeader header;    ///< common ACPI header ("HPET")
    uint32_t  block_id;  ///< +36: hardware rev / vendor / comparator count / caps

    // +40: Base Address as an ACPI Generic Address Structure (12 bytes).
    uint8_t  base_addr_space;      ///< +40: 0 = system memory
    uint8_t  base_reg_bit_width;   ///< +41
    uint8_t  base_reg_bit_offset;  ///< +42
    uint8_t  base_access_size;     ///< +43
    uint64_t base_address;         ///< +44: MMIO physical base (0xFED00000 on QEMU)

    uint8_t  hpet_number;      ///< +52: which HPET block this is (usually 0)
    uint16_t min_tick;         ///< +53: vendor-recommended min periodic tick
    uint8_t  page_protection;  ///< +55: I/O page-protection hints
};
static_assert(sizeof(HPETHeader) == 56, "ACPI HPET table must be 56 bytes");

/// Decoded HPET description table consumed by the F5-M4 MMIO driver.
struct HPETInfo {
    uint64_t address;  ///< MMIO physical base (0xFED00000 on QEMU); 0 if absent
    bool     present;  ///< a memory-space HPET block was decoded
};

/// Parse the HPET description table into HPETInfo.
///
/// Validates the table length and that the base address is in system-memory
/// space (address_space_id == 0), then returns the MMIO physical base.  A null
/// or too-short table yields present=false / address=0.
///
/// @param hpet  HPET via the direct map (from find_table("HPET")), or nullptr.
/// @return      Decoded info; present=false if the table is absent/invalid.
HPETInfo parse_hpet(const SDTHeader* hpet);

/// Decoded MADT information filled by init(); consumed by M2 (APIC).
extern ACPIInfo g_acpi_info;

/// Locate the RSDP, parse the MADT, and populate g_acpi_info.
///
/// Logs a one-line probe (CPU count, LAPIC/IOAPIC bases, IRQ overrides) so the
/// M2 APIC bring-up has a visible starting point.  Only needs the direct map,
/// so it can run early in boot.
void init();

}  // namespace cinux::drivers::acpi
