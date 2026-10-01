/**
 * @file    e820.hpp
 * @brief   E820 memory-map contract shared by boot and host tests.
 *
 * @author  Charliechen114514
 * @date    2026-09-27
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_e820
 * @copyright Copyright (c) 2026
 */

#pragma once
// stdint.h (not <cstdint>): the libstdc++ wrapper pulls glibc headers
// that do not survive -m16; the GCC C header is freestanding-clean.
#include <stdint.h>

namespace cinux::boot {

/**
 * @brief   Classification of one E820 entry.
 * @note    None
 * @since   0.1.0
 * @ingroup boot_e820
 */
enum class EntryType : uint8_t {
    kUsable          = 1,
    kReserved        = 2,
    kAcpiReclaimable = 3,
    kAcpiNvs         = 4,
    kBad             = 5,
};

struct [[gnu::packed]] MemoryMapEntry {
    unsigned long long base;
    unsigned long long length;
    unsigned int       type;       // Raw types of MapEntry
    unsigned int       acpi_attr;  // ACPI 3.0
};

inline constexpr unsigned short kE820MaxEntries = 32;

/**
 * @brief   Archive of BIOS-reported E820 entries.
 * @note    Lives in stage2 .bss and is filled at run time by the BIOS;
 *          nothing assumes it starts zeroed.
 * @since   0.1.0
 * @ingroup boot_e820
 */
struct MemoryMap {
    MemoryMapEntry entries[kE820MaxEntries];
    unsigned int   count;
};

// ABI layout pins: test_e820 carries the wire-size assertions in the host world

/**
 * @brief         Maps a raw BIOS type value to its classification.
 *
 * @param[in]     raw_type   Type field exactly as the BIOS wrote it.
 * @return        The classification; unknown values fall back to kReserved.
 * @note          Conservative by design: treating usable memory as
 *                reserved only wastes pages, the reverse steps on holes.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_e820
 */
constexpr EntryType ClassifyEntry(unsigned int raw_type) {
    switch (raw_type) {
    case 1:
        return EntryType::kUsable;
    case 3:
        return EntryType::kAcpiReclaimable;
    case 4:
        return EntryType::kAcpiNvs;
    case 5:
        return EntryType::kBad;
    case 2:
    default:
        return EntryType::kReserved;
    }
}

/**
 * @brief         Reports whether an entry may be handed out as free memory.
 *
 * @param[in]     type   Classification to test.
 * @return        true only for kUsable.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_e820
 */
constexpr bool IsUsable(EntryType type) {
    return type == EntryType::kUsable;
}

}  // namespace cinux::boot
