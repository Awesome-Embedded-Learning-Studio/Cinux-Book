#include "bios.hpp"
#include "cinux/ptr.hpp"
#include "e820/e820.hpp"
#include "early/console.hpp"
#include "early/print.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/image_header.hpp"
#include "layout.hpp"
#include "load/loader.hpp"
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
using namespace cinux::base;
using namespace cinux::boot;
using namespace cinux::console;
using cinux::print::Println;

cinux::boot::BootInfo g_boot_info;

constexpr unsigned int kMaxModesToProbe  = 128;
constexpr unsigned int kMaxUsableRegions = 16;

constexpr unsigned short kWantWidth  = 1024;
constexpr unsigned short kWantHeight = 768;
constexpr unsigned char  kBppPrimary = 32;
constexpr unsigned char  kBppSpare   = 24;

[[noreturn]] void fail(char const* what) {
    Println("%s FAILED", what);
    cinux::console::Halt();
}

void dump_memory_map(cinux::boot::MemoryMap const& map) {
    Println("E820: %u entries", map.count);
    for (unsigned int i = 0; i < map.count; ++i) {
        MemoryMapEntry const& entry = map.entries[i];
        // %016llX 零填充定宽:十六列 base 一眼对上,当年 GDB 里对地址的读法
        Println("  #%02X base=%016llX len=%016llX type=%X", i, entry.base, entry.length,
                entry.type);
    }
}

struct ModeChoice {
    unsigned short             number;
    cinux::boot::ModeInfoBlock info;
};

unsigned int count_mode_list(unsigned short const* mode_list) {
    unsigned int count = 0;
    while (count < kMaxModesToProbe && mode_list[count] != kModeListTerminator) {
        ++count;
    }
    return count;
}

bool probe_video_mode(unsigned short const* mode_list, ModeChoice& out) {
    cinux::boot::ModeInfoBlock probe{};
    ModeChoice                 fallback{};
    bool                       have_fallback = false;
    for (unsigned int i = 0; i < kMaxModesToProbe; ++i) {
        unsigned short const kNumber = mode_list[i];
        if (kNumber == kModeListTerminator) {
            break;
        }
        if (!bios::QueryModeInfo(kNumber, &probe)) {
            continue;
        }
        if (MatchesRequest(probe, kWantWidth, kWantHeight, kBppPrimary)) {
            out.number = kNumber;
            out.info   = probe;
            return true;
        }
        if (!have_fallback && MatchesRequest(probe, kWantWidth, kWantHeight, kBppSpare)) {
            fallback.number = kNumber;
            fallback.info   = probe;
            have_fallback   = true;
        }
    }
    if (have_fallback) {
        out = fallback;
    }
    return have_fallback;
}

cinux::boot::FrameBufferInfo describe_framebuffer(cinux::boot::ModeInfoBlock const& mode) {
    return {.physical = mode.framebuffer,
            .pitch    = mode.pitch,
            .width    = mode.width,
            .height   = mode.height,
            .bpp      = mode.bpp};
}

unsigned int collect_usable_regions(cinux::boot::MemoryMap const& map, cinux::boot::Region* usable,
                                    unsigned int capacity) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < map.count && count < capacity; ++i) {
        cinux::boot::MemoryMapEntry const& entry = map.entries[i];
        if (IsUsable(ClassifyEntry(entry.type))) {
            usable[count].base = entry.base;
            usable[count].top  = entry.base + entry.length;
            ++count;
        }
    }
    return count;
}

void fill_boot_info(cinux::boot::ImageHeader const& header, cinux::boot::MemoryMap const& map,
                    cinux::boot::FrameBufferInfo const& framebuffer,
                    cinux::boot::ModeInfoBlock const&   mode) {
    cinux::boot::BootInfo& info = g_boot_info;
    info.magic                  = cinux::boot::kBootInfoMagic;
    info.version                = cinux::boot::kBootInfoVersion;
    info.struct_size            = sizeof(cinux::boot::BootInfo);
    info.e820_count             = map.count;
    for (unsigned int i = 0; i < map.count && i < cinux::boot::kBootInfoE820Max; ++i) {
        info.e820[i].base   = map.entries[i].base;
        info.e820[i].length = map.entries[i].length;
        info.e820[i].type   = map.entries[i].type;
    }
    info.framebuffer.physical    = framebuffer.physical;
    info.framebuffer.pitch       = framebuffer.pitch;
    info.framebuffer.width       = framebuffer.width;
    info.framebuffer.height      = framebuffer.height;
    info.framebuffer.bpp         = framebuffer.bpp;
    info.framebuffer.red_size    = mode.red_mask_size;
    info.framebuffer.red_shift   = mode.red_field_position;
    info.framebuffer.green_size  = mode.green_mask_size;
    info.framebuffer.green_shift = mode.green_field_position;
    info.framebuffer.blue_size   = mode.blue_mask_size;
    info.framebuffer.blue_shift  = mode.blue_field_position;
    info.kernel_paddr            = header.load_paddr;
    info.kernel_file_size        = header.file_size;
    info.kernel_mem_size         = header.mem_size;
    info.kernel_entry            = header.entry;
}

cinux::boot::ImageHeader load_kernel_image(cinux::boot::MemoryMap const& map) {
    cinux::boot::Region       usable[kMaxUsableRegions];
    unsigned int const        kUsableCount = collect_usable_regions(map, usable, kMaxUsableRegions);
    cinux::boot::Region const kOwned[]     = {
        {.base = 0, .top = kFerryWindow + kFerryWindowSize},
        {.base = kStage2Spot.offset,
         .top  = kStage2Spot.offset + cinux::boot::load::SectorBytes(kStage2Spot.sectors)},
        {.base = kPageTables.base, .top = kPageTables.top},
        {.base = kStage2Stack.top - 0x1000, .top = kPmStackTop},
    };
    cinux::boot::ImageHeader const kHeader = cinux::boot::load::ReadHeader();
    cinux::boot::LoadStatus const  kVerdict =
        cinux::boot::ValidateImage(kHeader, usable, kUsableCount, kOwned, 4);
    if (kVerdict != cinux::boot::LoadStatus::kOk) {
        Println("[stage2] kernel image rejected: %s", cinux::boot::NameOf(kVerdict));
        cinux::console::Halt();
    }
    cinux::console::PutString("[stage2] kernel image ok\n");
    cinux::boot::LoadStatus const kLoaded = cinux::boot::load::LoadKernel(kHeader);
    if (kLoaded != cinux::boot::LoadStatus::kOk) {
        Println("[stage2] kernel load failed: %s", cinux::boot::NameOf(kLoaded));
        cinux::console::Halt();
    }
    cinux::console::PutString("[stage2] kernel ferried\n");
    return kHeader;
}

}  // namespace

extern "C" {
void* g_kernel_boot_info = &g_boot_info;
}

extern "C" [[noreturn]] void Stage2Main() {
    using namespace cinux::boot;
    cinux::console::InitConsole();
    cinux::console::PutString("[stage2] stack ok\n");

    if (!bios::EnableA20AddressLine()) {
        fail("A20");
    }
    cinux::console::PutString("A20 ok\n");

    cinux::boot::MemoryMap memory_map{};
    if (!bios::CollectMemoryMap(&memory_map)) {
        fail("E820");
    }
    dump_memory_map(memory_map);

    cinux::boot::VbeInfoBlock vbe_info{};
    if (!bios::QueryControllerInfo(&vbe_info)) {
        fail("VESA ctrl");
    }

    // Real-mode seg:off far pointer flattened to a DS=0 linear address; the BIOS owns the target.
    auto const* mode_list = PtrAt<unsigned short>(
        (static_cast<unsigned long>(vbe_info.mode_list_segment) << 4) + vbe_info.mode_list_offset);

    ModeChoice choice{};
    if (!probe_video_mode(mode_list, choice)) {
        fail("VESA mode");
    }
    if (!bios::SetVideoMode(choice.number)) {
        fail("VESA set");
    }

    cinux::boot::FrameBufferInfo const kFramebuffer = describe_framebuffer(choice.info);
    Println("VESA: VBE 0x%04X %u modes", vbe_info.version, count_mode_list(mode_list));
    Println("VESA: 0x%04X %u*%u*%u LFB", choice.number | kLinearFrameBufferFlag, kFramebuffer.width,
            kFramebuffer.height, kFramebuffer.bpp);
    Println("  fb %016llX pitch %u", kFramebuffer.physical, kFramebuffer.pitch);

    cinux::boot::ImageHeader const kHeader = load_kernel_image(memory_map);
    fill_boot_info(kHeader, memory_map, kFramebuffer, choice.info);
    StoreWord(kHandoffMailboxEntry, static_cast<unsigned long>(kHeader.entry));
    StoreWord(kHandoffMailboxInfo, reinterpret_cast<unsigned long>(g_kernel_boot_info));

    cinux::console::PutString("[stage2] leaving real mode\n");
    EnterProtectedMode();
}
