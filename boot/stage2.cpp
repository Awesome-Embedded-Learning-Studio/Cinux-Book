#include <cinux/format.hpp>

#include "bios.hpp"
#include "e820/e820.hpp"
#include "early/boot_console.hpp"
#include "layout.hpp"
#include "vesa/vesa.hpp"

extern "C" [[noreturn]] void EnterProtectedMode();

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

cinux::boot::VbeInfoBlock    g_vbe_info;
cinux::boot::ModeInfoBlock   g_mode_scratch;
cinux::boot::ModeInfoBlock   g_mode_chosen;
cinux::boot::FrameBufferInfo g_framebuffer;

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
                      sizeof(cinux::boot::MemoryMap) + sizeof(cinux::boot::VbeInfoBlock) +
                      (sizeof(cinux::boot::ModeInfoBlock) * 2U) +
                      sizeof(cinux::boot::FrameBufferInfo),
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

    if (!bios::QueryControllerInfo(&g_vbe_info)) {
        fail("VESA ctrl");
    }

    // NOLINTBEGIN(performance-no-int-to-ptr)
    // Real-mode seg:off far pointer flattened to a DS=0 linear address; the BIOS owns the target.
    auto const* mode_list = reinterpret_cast<unsigned short const*>(
        (static_cast<unsigned long>(g_vbe_info.mode_list_segment) << 4) +
        g_vbe_info.mode_list_offset);
    // NOLINTEND(performance-no-int-to-ptr)

    constexpr unsigned int kMaxModesToProbe = 128;
    unsigned int           seen             = 0;
    unsigned short         picked32         = 0xFFFF;
    unsigned short         picked24         = 0xFFFF;
    for (unsigned int i = 0; i < kMaxModesToProbe; ++i) {
        unsigned short const kNumber = mode_list[i];
        if (kNumber == 0xFFFF) {
            break;
        }
        ++seen;
        if (!bios::QueryModeInfo(kNumber, &g_mode_scratch)) {
            continue;
        }
        if (picked32 == 0xFFFF && MatchesRequest(g_mode_scratch, 1024, 768, 32)) {
            picked32      = kNumber;
            g_mode_chosen = g_mode_scratch;
        } else if (picked24 == 0xFFFF && MatchesRequest(g_mode_scratch, 1024, 768, 24)) {
            picked24 = kNumber;
            if (picked32 == 0xFFFF) {
                g_mode_chosen = g_mode_scratch;
            }
        }
    }

    unsigned short const kChosen = picked32 != 0xFFFF ? picked32 : picked24;
    if (kChosen == 0xFFFF) {
        fail("VESA mode");
    }
    if (!bios::SetVideoMode(kChosen)) {
        fail("VESA set");
    }

    g_framebuffer.physical = g_mode_chosen.framebuffer;
    g_framebuffer.pitch    = g_mode_chosen.pitch;
    g_framebuffer.width    = g_mode_chosen.width;
    g_framebuffer.height   = g_mode_chosen.height;
    g_framebuffer.bpp      = g_mode_chosen.bpp;

    {
        char              buf[64];
        format::Arg const kArgs[] = {format::NumU(g_vbe_info.version), format::NumU(seen)};
        format::FormatToBuf(buf, sizeof(buf), "VESA: VBE 0x%04X %u modes\n",
                            format::ArgsView::of(kArgs));
        serial::PutString(buf);
    }
    {
        char              buf[64];
        format::Arg const kArgs[] = {
            format::NumU(kChosen | kLinearFrameBufferFlag), format::NumU(g_framebuffer.width),
            format::NumU(g_framebuffer.height), format::NumU(g_framebuffer.bpp)};
        format::FormatToBuf(buf, sizeof(buf), "VESA: 0x%04X %u*%u*%u LFB\n",
                            format::ArgsView::of(kArgs));
        serial::PutString(buf);
    }
    {
        char              buf[64];
        format::Arg const kArgs[] = {format::NumU(g_framebuffer.physical),
                                     format::NumU(g_framebuffer.pitch)};
        format::FormatToBuf(buf, sizeof(buf), "  fb %016llX pitch %u\n",
                            format::ArgsView::of(kArgs));
        serial::PutString(buf);
    }

    serial::PutString("[stage2] leaving real mode\n");
    EnterProtectedMode();
}
