#include <cinux/format.hpp>

#include "bios.hpp"
#include "e820/e820.hpp"
#include "early/boot_console.hpp"
#include "layout.hpp"

asm(".section .text.boot, \"ax\"\n"
    ".global stage2_start\n"
    "stage2_start:\n"
    "   cli\n"
    "   xor %ax, %ax\n"
    "   mov %ax, %ds\n"
    "   mov %ax, %es\n"
    "   mov %ax, %ss\n"
    "   movw $0x7000, %sp\n"  // Can we trust the new envs? better not :)
    "   cld\n"
    "   call Stage2Main\n"  // OK, call and enter
    "0: hlt\n"              // And that is always the hlt
    "   jmp 0b\n"
    ".text\n");

namespace {
using namespace cinux::boot;
using namespace cinux::base;

cinux::boot::MemoryMap g_memory_map;

[[noreturn]] void fail(char const* what) {
    char              buf[48];
    format::Arg const kArgs[] = {format::Str(what)};
    format::FormatToBuf(buf, sizeof(buf), "%s FAILED\n", format::ArgsView::of(kArgs));
    serial::PutString(buf);
    for (;;) {
        asm volatile("hlt");
    }
}

void dump_memory_map(cinux::boot::MemoryMap const& map) {
    static constexpr unsigned short kFormatBufferSize = 72;
    char                            buf[kFormatBufferSize];
    format::Arg const               kHeader[] = {format::NumU(map.count)};
    format::FormatToBuf(buf, sizeof(buf), "E820: %u entries\n", format::ArgsView::of(kHeader));
    serial::PutString(buf);
    for (unsigned int i = 0; i < map.count; ++i) {
        MemoryMapEntry const& entry   = map.entries[i];
        // %016llX 零填充定宽:十六列 base 一眼对上,当年 GDB 里对地址的读法
        format::Arg const     kArgs[] = {format::NumU(i), format::NumU(entry.base),
                                         format::NumU(entry.length), format::NumU(entry.type)};
        format::FormatToBuf(buf, sizeof(buf), "  #%02X base=%016llX len=%016llX type=%X\n",
                            format::ArgsView::of(kArgs));
        serial::PutString(buf);
    }
}
}  // namespace

static_assert(static_cast<unsigned long>(cinux::boot::kKernelLoadLma) >
                  static_cast<unsigned long>(cinux::boot::kStage2Spot.offset) +
                      (static_cast<unsigned long>(cinux::boot::kStage2Spot.sectors) * 512U) +
                      sizeof(cinux::boot::MemoryMap),
              "Guard: kernel load address overlaps stage2 footprint");

extern "C" [[noreturn]] void Stage2Main() {
    using namespace cinux::boot;
    serial::PutString("[stage2] stack ok\n");

    if (!bios::EnableA20AddressLine()) {
        fail("A20");
    }
    serial::PutString("A20 ok\n");

    if (bios::CollectMemoryMap(&g_memory_map) != 0) {
        fail("E820");
    }

    dump_memory_map(g_memory_map);

    serial::PutString("[stage2] alive\n");
    for (;;) {
        asm volatile("hlt");
    }
}
