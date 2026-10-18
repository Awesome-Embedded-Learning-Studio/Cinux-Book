/**
 * @file    acpi.cpp
 * @brief   The walk itself: EBDA sweep, firmware sweep, table hop.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/acpi/acpi.hpp"

#include <stdint.h>

#include <cstddef>

#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "kernel/driver/base/io.hpp"
#include "kernel/mm/layout.hpp"

namespace cinux::driver::acpi {

static_assert(offsetof(Rsdp, rsdt_address) == 16);
static_assert(offsetof(Rsdp, xsdt_address) == 24);
static_assert(offsetof(TableHeader, length) == 4);
static_assert(offsetof(Fadt, pm1a_control) == 64);

namespace {

constexpr unsigned long kScanStep         = 16;
constexpr unsigned long kEbdaBytes        = 1024;
constexpr unsigned long kLowWindowPhys    = 0xE0000;
constexpr unsigned long kLowWindowBytes   = 0xFFFFF - kLowWindowPhys + 1;
constexpr unsigned long kEbdaBasePhysPort = 0x40E;
constexpr unsigned long kHeaderBytes      = 36;

/// Sleep type zero, sleep-enable bit thirteen: QEMU's firmware family
/// maps the S5 package onto zero, and its PM model cuts power there.
constexpr uint16_t kS5Word = static_cast<uint16_t>((0UL << 10U) | (1UL << 13U));

bool bytes_match(const void* where, const char* wanted, unsigned long length) {
    return base::EqualBytes(static_cast<const unsigned char*>(where),
                            reinterpret_cast<const unsigned char*>(wanted), length);
}

const Rsdp* rsdp_at(unsigned long virt) {
    const auto* const kMaybe =
        reinterpret_cast<const Rsdp*>(virt);  // NOLINT(performance-no-int-to-ptr)
    if (!bytes_match(kMaybe, "RSD PTR ", 7)) {
        return nullptr;
    }
    const unsigned long kFirst20 =
        sizeof(Rsdp{}.signature) + sizeof(uint8_t) + 6UL + sizeof(uint8_t) + sizeof(uint32_t);
    const auto* const kBytes = reinterpret_cast<const unsigned char*>(kMaybe);
    unsigned char     sum    = 0;
    for (unsigned long index = 0; index < kFirst20; ++index) {
        sum = static_cast<unsigned char>(sum + kBytes[index]);
    }
    if (sum != 0) {
        return nullptr;
    }
    return kMaybe;
}

const Rsdp* scan_for_root(unsigned long virt, unsigned long bytes) {
    for (unsigned long offset = 0; offset + sizeof(Rsdp) <= bytes; offset += kScanStep) {
        if (const Rsdp* const kFound = rsdp_at(virt + offset); kFound != nullptr) {
            return kFound;
        }
    }
    return nullptr;
}

}  // namespace

bool TableSumsToZero(const void* bytes, unsigned long length) {
    const auto* const kBytes = static_cast<const unsigned char*>(bytes);
    unsigned char     sum    = 0;
    for (unsigned long index = 0; index < length; ++index) {
        sum = static_cast<unsigned char>(sum + kBytes[index]);
    }
    return sum == 0;
}

const Rsdp* FindRootPointer() {
    const auto* const kSegmentBytes =
        reinterpret_cast<const unsigned char*>(  // NOLINT(performance-no-int-to-ptr)
            mm::DirectMapVirt(kEbdaBasePhysPort));
    const unsigned long kEbdaSegment = base::ReadWord16(kSegmentBytes, 0);
    if (kEbdaSegment != 0) {
        const unsigned long kEbdaPhys = kEbdaSegment << 4U;
        if (const Rsdp* const kFound = scan_for_root(mm::DirectMapVirt(kEbdaPhys), kEbdaBytes);
            kFound != nullptr) {
            return kFound;
        }
    }
    return scan_for_root(mm::DirectMapVirt(kLowWindowPhys), kLowWindowBytes);
}

const TableHeader* FindTable(const Rsdp* root, const char* signature) {
    if (root == nullptr) {
        return nullptr;
    }
    unsigned long directory_phys = root->rsdt_address;
    unsigned long entry_bytes    = sizeof(uint32_t);
    if (root->revision >= 2) {
        const auto* const kRootBytes = reinterpret_cast<const unsigned char*>(root) + 24U;
        directory_phys = base::ReadWord32(kRootBytes, 0) |
                         (static_cast<unsigned long>(base::ReadWord32(kRootBytes, 4)) << 32U);
        entry_bytes    = sizeof(uint64_t);
    }
    const auto* const kDirectory =
        reinterpret_cast<const TableHeader*>(  // NOLINT(performance-no-int-to-ptr)
            mm::DirectMapVirt(directory_phys));
    if (!TableSumsToZero(kDirectory, kDirectory->length)) {
        return nullptr;
    }
    const auto* const kEntries = reinterpret_cast<const unsigned char*>(kDirectory) + kHeaderBytes;
    const unsigned long kCount = (kDirectory->length - kHeaderBytes) / entry_bytes;
    for (unsigned long index = 0; index < kCount; ++index) {
        const auto* const kEntryBytes = kEntries + (index * entry_bytes);
        unsigned long     entry_phys  = base::ReadWord32(kEntryBytes, 0);
        if (entry_bytes == sizeof(uint64_t)) {
            entry_phys =
                entry_phys | (static_cast<unsigned long>(base::ReadWord32(kEntryBytes, 4)) << 32U);
        }
        const auto* const kTable =
            reinterpret_cast<const TableHeader*>(  // NOLINT(performance-no-int-to-ptr)
                mm::DirectMapVirt(entry_phys));
        if (!bytes_match(kTable, signature, 4) || !TableSumsToZero(kTable, kTable->length)) {
            continue;
        }
        return kTable;
    }
    return nullptr;
}

const Fadt* FindFadt(const Rsdp* root) {
    return reinterpret_cast<const Fadt*>(FindTable(root, "FACP"));
}

void PowerOff(const Fadt* fadt) {
    if (fadt == nullptr) {
        return;
    }
    Out16(PortWrite16{.port = static_cast<uint16_t>(fadt->pm1a_control), .value = kS5Word});
}

}  // namespace cinux::driver::acpi
