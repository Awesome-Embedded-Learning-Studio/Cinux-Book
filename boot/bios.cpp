#include "bios.hpp"

#include <stdint.h>

#include "e820/e820.hpp"

namespace {

constexpr uint32_t kQueryMemoryMapMagic = 0xE820;
constexpr uint32_t kSmapSignature       = 0x534D4150;

using cinux::boot::MemoryMapEntry;

constexpr uint32_t kMinimumEntryBytes = sizeof(MemoryMapEntry) - sizeof(MemoryMapEntry::acpi_attr);

bool query_one(uint32_t& continuation, MemoryMapEntry* entry) {
    // NOLINTBEGIN(misc-const-correctness)
    // Operands bound to asm read-write/output constraints cannot be const.
    uint32_t       register_ax = kQueryMemoryMapMagic;
    uint32_t       register_cx = sizeof(MemoryMapEntry);
    const uint32_t kRegisterDx = kSmapSignature;
    uint8_t        carry_flag;
    // NOLINTEND(misc-const-correctness)

    asm volatile(
        "pushw %%ds\n"
        "popw %%es\n"
        "int $0x15\n"
        "setc %[carry_flag]"
        : [ax] "+a"(register_ax), [continuation] "+b"(continuation), [bytes] "+c"(register_cx),
          [carry_flag] "=q"(carry_flag)
        : [signature] "d"(kRegisterDx), [entry] "D"(entry)
        : "memory");

    return carry_flag == 0 && register_ax == kSmapSignature && register_cx >= kMinimumEntryBytes;
}
}  // namespace

namespace cinux::boot::bios {

bool EnableA20AddressLine() {
    static constexpr unsigned short kEnableA20Magic = 0x2401;
    // NOLINTBEGIN(misc-const-correctness)
    // Operands bound to asm read-write/output constraints cannot be const.
    unsigned short                  register_ax     = kEnableA20Magic;
    unsigned char                   enable_a20_ok;
    // NOLINTEND(misc-const-correctness)
    asm volatile(
        "int $0x15\n"
        "setnc %1"  // CF=0(成功)时置 1——A20 与 E820 的 carry 语义相反
        : "+a"(register_ax), "=q"(enable_a20_ok)
        :
        : "memory");
    return enable_a20_ok != 0;
}


int CollectMemoryMap(MemoryMap* map) {
    map->count            = 0;
    uint32_t continuation = 0;
    do {
        if (!query_one(continuation, &map->entries[map->count])) {
            return 1;
        }
        ++map->count;
        if (map->count >= kE820MaxEntries) {
            break;
        }
    } while (continuation != 0);
    return 0;
}


}  // namespace cinux::boot::bios
