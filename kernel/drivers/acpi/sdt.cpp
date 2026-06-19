/**
 * @file kernel/drivers/acpi/sdt.cpp
 * @brief ACPI RSDT/XSDT walking and signature lookup (F4-M1)
 *
 * Implements find_table(): follow the RSDP to its root table (XSDT for ACPI
 * 2.0+, RSDT for 1.0), walk the entry array, and return the first valid table
 * whose signature matches.  QEMU's default 'pc' machine exposes an ACPI 1.0
 * RSDP (rev 0), so the RSDT path with 32-bit physical pointers is the one
 * actually exercised.
 *
 * Entry pointers are read byte-wise so an unaligned entry array (the entries
 * start right after the 36-byte SDT header, which is not 8-byte aligned) does
 * not trip -Wcast-align.
 */

#include <stddef.h>
#include <stdint.h>

#include "acpi.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"

namespace cinux::drivers::acpi {

namespace {

// Reject tables larger than this before checksumming, so a corrupt length
// field cannot make validate_checksum scan megabytes.  MADT/FADT/HPET are all
// well under 64 KB.
constexpr size_t kMaxTableBytes = 0x10000;

const SDTHeader* phys_sdt(uint64_t phys) {
    return reinterpret_cast<const SDTHeader*>(phys_to_virt(phys));
}

/// Compare two 4-byte signatures (not NUL-terminated).
bool sig4_matches(const char* a, const char* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64_le(const uint8_t* p) {
    return static_cast<uint64_t>(read_u32_le(p)) |
           (static_cast<uint64_t>(read_u32_le(p + 4)) << 32);
}

/// Walk one root table's entry array and return the first valid table whose
/// signature matches.  @p entry_size is 4 (RSDT) or 8 (XSDT).
const SDTHeader* walk_root(const SDTHeader* root, const char* signature, size_t entry_size) {
    if (root == nullptr || root->length < sizeof(SDTHeader) || root->length > kMaxTableBytes) {
        return nullptr;
    }
    if (!validate_checksum(root, root->length)) {
        return nullptr;
    }
    const size_t   count   = (root->length - sizeof(SDTHeader)) / entry_size;
    const uint8_t* entries = reinterpret_cast<const uint8_t*>(root) + sizeof(SDTHeader);
    for (size_t i = 0; i < count; ++i) {
        const uint64_t phys =
            (entry_size == 4) ? read_u32_le(entries + i * 4) : read_u64_le(entries + i * 8);
        if (phys == 0) {
            continue;
        }
        const SDTHeader* t = phys_sdt(phys);
        if (sig4_matches(t->signature, signature) && t->length >= sizeof(SDTHeader) &&
            t->length <= kMaxTableBytes && validate_checksum(t, t->length)) {
            return t;
        }
    }
    return nullptr;
}

}  // namespace

const SDTHeader* find_table(const char* signature) {
    const RSDP* rsdp = find_rsdp();
    if (rsdp == nullptr) {
        return nullptr;
    }
    // Prefer XSDT (ACPI 2.0+) when present; otherwise the RSDT (32-bit) is the
    // only option.  QEMU's default 'pc' machine has rev 0 -> RSDT only.
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        const SDTHeader* found = walk_root(phys_sdt(rsdp->xsdt_address), signature, 8);
        if (found != nullptr) {
            return found;
        }
    }
    if (rsdp->rsdt_address != 0) {
        return walk_root(phys_sdt(rsdp->rsdt_address), signature, 4);
    }
    return nullptr;
}

}  // namespace cinux::drivers::acpi
