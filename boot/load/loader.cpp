#include "load/loader.hpp"

#include <stdint.h>

#include "cinux/ptr.hpp"
#include "kernel/boot/image_header.hpp"
#include "layout.hpp"
#include "mbr/disk_address_packet.hpp"

namespace cinux::boot::load {

extern "C" {
extern unsigned g_dst;
extern unsigned g_src;
extern unsigned g_len;
}


namespace {
Dap g_window_dap = {.size     = 16,
                    .reserved = 0,
                    .count    = kFerryWindowSectors,
                    .offset   = 0,
                    .segment  = static_cast<unsigned short>(kFerryWindow >> 4),
                    .lba      = kKernelImageLba};
}  // namespace

extern "C" unsigned long g_kernel_entry;
extern "C" unsigned long g_kernel_end_paddr;
unsigned long            g_kernel_entry;
unsigned long            g_kernel_end_paddr;

namespace {

bool read_window() {
    // NOLINTBEGIN(misc-const-correctness)
    unsigned short       register_ax = 0x4200;
    // NOLINTEND(misc-const-correctness)
    unsigned short const kRegisterDx = 0x80;
    asm volatile("int $0x13" : "+a"(register_ax) : "S"(&g_window_dap), "d"(kRegisterDx) : "memory");
    return (register_ax >> 8) == 0;
}

}  // namespace

ImageHeader ReadHeader() {
    asm volatile("lgdt kGdtr" : : : "memory");
    g_window_dap.count = 1;
    read_window();
    RunFerry(kHeaderScratch, static_cast<unsigned>(kFerryWindow), 512);
    return *base::PtrAt<ImageHeader>(kHeaderScratch);
}

LoadStatus LoadKernel(const ImageHeader& header) {
    uint64_t remaining    = header.file_size;
    uint64_t lba          = kKernelImageLba;
    uint64_t paddr        = header.load_paddr;
    bool     first_window = true;
    while (remaining > 0) {
        auto const kSectors = SectorsFor(remaining, kFerryWindowSectors);
        g_window_dap.count  = static_cast<unsigned short>(kSectors);
        g_window_dap.lba    = lba;
        if (!read_window()) {
            return LoadStatus::kDiskError;
        }
        unsigned long const kBytes = SectorBytes(kSectors);
        unsigned long const kChunk =
            remaining < kBytes ? static_cast<unsigned long>(remaining) : kBytes;
        RunFerry(static_cast<unsigned>(paddr), static_cast<unsigned>(kFerryWindow),
                 static_cast<unsigned>(kChunk));
        if (first_window) {
            RunFerry(kHeaderScratch, static_cast<unsigned>(paddr), 8);
            if (LoadWord(kHeaderScratch) != kImageMagic) {
                return LoadStatus::kMagicMismatch;
            }
            first_window = false;
        }
        lba += kSectors;
        paddr += SectorBytes(kSectors);
        remaining -= static_cast<uint64_t>(kChunk);
    }
    g_kernel_entry     = static_cast<unsigned long>(header.entry);
    g_kernel_end_paddr = static_cast<unsigned long>(header.load_paddr + header.mem_size);
    return LoadStatus::kOk;
}

}  // namespace cinux::boot::load
