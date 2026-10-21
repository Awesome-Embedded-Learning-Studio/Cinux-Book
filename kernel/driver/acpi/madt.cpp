#include "kernel/driver/acpi/madt.hpp"

#include <stdint.h>

#include <cstddef>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/acpi/madt_config.hpp"

namespace cinux::driver::acpi {

namespace {

struct Madt {
    TableHeader header;
    uint32_t    local_apic_address;
    uint32_t    flags;
};
struct EntryHeader {
    uint8_t type;
    uint8_t length;
};
struct ProcessorEntry {
    EntryHeader header;
    uint8_t     processor_id;
    uint8_t     apic_id;
    uint32_t    flags;
};
struct IoApicEntry {
    EntryHeader header;
    uint8_t     id;
    uint8_t     reserved;
    uint32_t    address;
    uint32_t    gsi_base;
};
struct [[gnu::packed]] OverrideEntry {
    EntryHeader header;
    uint8_t     bus;
    uint8_t     source;
    uint32_t    gsi;
    uint16_t    flags;
};
static_assert(sizeof(Madt) == 44);
static_assert(offsetof(Madt, local_apic_address) == 36);
static_assert(sizeof(ProcessorEntry) == 8 && offsetof(ProcessorEntry, flags) == 4);
static_assert(sizeof(IoApicEntry) == 12 && offsetof(IoApicEntry, address) == 4);
static_assert(sizeof(OverrideEntry) == 10 && offsetof(OverrideEntry, flags) == 8);

template <typename Wire>
Wire read_wire(const unsigned char* bytes) {
    Wire value{};
    base::CopyBytes(&value, bytes, sizeof(value));
    return value;
}

constexpr unsigned int  kHeaderBytes       = sizeof(Madt);
constexpr unsigned int  kMaximumTableBytes = 65536;
constexpr unsigned int  kEntryHeaderBytes  = 2;
constexpr unsigned char kProcessorEntry    = 0;
constexpr unsigned char kIoApicEntry       = 1;
constexpr unsigned char kOverrideEntry     = 2;
constexpr unsigned int  kProcessorBytes    = sizeof(ProcessorEntry);
constexpr unsigned int  kIoApicBytes       = sizeof(IoApicEntry);
constexpr unsigned int  kOverrideBytes     = sizeof(OverrideEntry);
constexpr auto          kEnabled           = base::bit::MaskBit<uint32_t>(0);
constexpr auto          kOnlineCapable     = base::bit::MaskBit<uint32_t>(1);
constexpr auto          kPcatCompatible    = base::bit::MaskBit<uint32_t>(0);

bool decode_entry(InterruptTopology& topology, const unsigned char* entry) {
    const auto         kHeader = read_wire<EntryHeader>(entry);
    const unsigned int kLength = kHeader.length;
    switch (kHeader.type) {
    case kProcessorEntry: {
        if (kLength < kProcessorBytes) {
            return false;
        }
        const auto                         kProcessor = read_wire<ProcessorEntry>(entry);
        const base::bit::BitMask<uint32_t> kFlags{kProcessor.flags};
        if (kFlags.has(kEnabled | kOnlineCapable) &&
            topology.processor_count < kMaxProcessorApics) {
            topology.processor_apic_ids[topology.processor_count++] = kProcessor.apic_id;
        }
        break;
    }
    case kIoApicEntry:
        if (kLength < kIoApicBytes) {
            return false;
        }
        if (topology.io_apic_address == 0) {
            const auto kIoApic        = read_wire<IoApicEntry>(entry);
            topology.io_apic_address  = kIoApic.address;
            topology.io_apic_gsi_base = kIoApic.gsi_base;
        }
        break;
    case kOverrideEntry: {
        if (kLength < kOverrideBytes) {
            return false;
        }
        const auto kOverride = read_wire<OverrideEntry>(entry);
        if (kOverride.bus == 0 && kOverride.source < kMaxIrqOverrides &&
            topology.override_count < kMaxIrqOverrides) {
            topology.overrides[topology.override_count++] = {
                .source = kOverride.source,
                .gsi    = kOverride.gsi,
                .flags  = kOverride.flags,
            };
        }
        break;
    }
    default:
        break;
    }
    return true;
}

}

InterruptTopology ParseMadt(const unsigned char* table, unsigned long bytes) {
    if (table == nullptr || bytes < kHeaderBytes ||
        !base::EqualBytes(table, reinterpret_cast<const unsigned char*>("APIC"), 4)) {
        return {};
    }
    const auto     kMadt   = read_wire<Madt>(table);
    const uint32_t kLength = kMadt.header.length;
    if (kLength < kHeaderBytes || kLength > bytes || kLength > kMaximumTableBytes) {
        return {};
    }
    InterruptTopology topology;
    topology.local_apic_address = kMadt.local_apic_address;
    topology.pcat_compatible    = base::bit::BitMask<uint32_t>{kMadt.flags}.has(kPcatCompatible);
    for (unsigned int offset = kHeaderBytes; offset < kLength;) {
        if (kLength - offset < kEntryHeaderBytes) {
            return {};
        }
        const unsigned int kEntryLength = read_wire<EntryHeader>(table + offset).length;
        if (kEntryLength < kEntryHeaderBytes || kEntryLength > kLength - offset ||
            !decode_entry(topology, table + offset)) {
            return {};
        }
        offset += kEntryLength;
    }
    return topology;
}

}
