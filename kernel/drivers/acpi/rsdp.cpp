/**
 * @file kernel/drivers/acpi/rsdp.cpp
 * @brief RSDP discovery and checksum validation (F4-M1)
 *
 * Implements validate_checksum() and find_rsdp().  Physical addresses are
 * reached through phys_to_virt() (the mini loader's identity direct map covers
 * all of low RAM where the RSDP and SDTs reside).
 */

#include <stddef.h>
#include <stdint.h>

#include "acpi.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::lib::kprintf;

namespace cinux::drivers::acpi {

bool validate_checksum(const void* table, size_t len) {
    const auto* p   = static_cast<const uint8_t*>(table);
    uint8_t     sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum = static_cast<uint8_t>(sum + p[i]);
    }
    return sum == 0;
}

namespace {

/// Reinterpret a physical address as a typed pointer through the direct map.
template <typename T>
const T* phys_cast(uint64_t phys) {
    return reinterpret_cast<const T*>(phys_to_virt(phys));
}

/// Byte-wise compare a fixed-width signature against the expected bytes.
bool signature_matches(const char* sig, const char* expected, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (sig[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

/// Verify the checksums of an RSDP candidate.
///
/// @return true if the ACPI 1.0 checksum (first 20 bytes) holds and, for
///         revision >= 2, the extended checksum over @ref RSDP::length bytes.
bool rsdp_checksums_ok(const RSDP* rsdp) {
    if (!validate_checksum(rsdp, 20)) {
        return false;
    }
    if (rsdp->revision >= 2) {
        // length is the whole RSDP size; the extended checksum covers it.
        if (rsdp->length < sizeof(RSDP)) {
            return false;
        }
        if (!validate_checksum(rsdp, rsdp->length)) {
            return false;
        }
    }
    return true;
}

/// Scan [start, end) in 16-byte strides for a valid RSDP.
const RSDP* probe_region(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr + sizeof(RSDP) <= end; addr += 16) {
        const RSDP* rsdp = phys_cast<RSDP>(addr);
        if (signature_matches(rsdp->signature, kRsdpSignature, sizeof(kRsdpSignature)) &&
            rsdp_checksums_ok(rsdp)) {
            return rsdp;
        }
    }
    return nullptr;
}

}  // namespace

const RSDP* find_rsdp() {
    const RSDP* rsdp = nullptr;

    // 1. EBDA: the real-mode segment is stored at physical 0x40E; shift left
    //    by 4 to get the linear base, then scan its first 1 KB.
    const uint16_t ebda_segment = *phys_cast<uint16_t>(0x40E);
    const uint64_t ebda_base    = static_cast<uint64_t>(ebda_segment) << 4;
    if (ebda_base != 0) {
        rsdp = probe_region(ebda_base, ebda_base + 1024);
    }

    // 2. BIOS ROM area 0xE0000-0xFFFFF.
    if (rsdp == nullptr) {
        rsdp = probe_region(0xE0000, 0x100000);
    }

    if (rsdp != nullptr) {
        // Report the revision so the M1-2 walker knows whether to follow the
        // 32-bit RSDT (rev 0) or the 64-bit XSDT (rev >= 2).  QEMU's default
        // 'pc' machine exposes rev 0 (ACPI 1.0, RSDT only, no XSDT).
        kprintf("[ACPI] RSDP found: rev %d, rsdt 0x%lX\n", static_cast<int>(rsdp->revision),
                static_cast<unsigned long>(rsdp->rsdt_address));
    } else {
        kprintf("[ACPI] RSDP not found\n");
    }
    return rsdp;
}

}  // namespace cinux::drivers::acpi
