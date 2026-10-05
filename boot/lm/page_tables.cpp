#include <stdint.h>

#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/boot/boot_info.hpp"
#include "layout.hpp"
#include "lm.hpp"

namespace {
using cinux::arch::page::Entry;
using cinux::arch::page::kHugePageSize;
using cinux::arch::page::kWritable;
using cinux::arch::page::MakeHugePageEntry;
using cinux::arch::page::MakeLargePageEntry;
using cinux::arch::page::MakeTableEntry;
using cinux::arch::page::PlanDeviceDoors;

using cinux::boot::lm::kPdPhys;
using cinux::boot::lm::kPdptPhys;
using cinux::boot::lm::kPml4Phys;

constexpr unsigned int kEntriesPerPage = 512;

auto* table_at(unsigned long physical) {
    return cinux::base::PtrAt<Entry>(physical);
}
}  // namespace

extern "C" unsigned long g_kernel_end_paddr;

extern "C" void BuildHandoffDoors() {
    auto* const kPml4 = table_at(kPml4Phys);
    auto* const kPdpt = table_at(kPdptPhys);
    auto* const kPd   = table_at(kPdPhys);

    unsigned long const kTableBytes = sizeof(Entry) * kEntriesPerPage;
    cinux::base::SetBytes(kPml4, 0, kTableBytes);
    cinux::base::SetBytes(kPdpt, 0, kTableBytes);
    cinux::base::SetBytes(kPd, 0, kTableBytes);

    unsigned long const kCovered   = g_kernel_end_paddr + 0x1FFFFFUL;
    auto const          kDoorCount = static_cast<unsigned int>(kCovered >> 21);
    for (unsigned int i = 0; i < kDoorCount; ++i) {
        kPd[i] = MakeLargePageEntry(i * cinux::arch::page::kLargePageSize, kWritable);
    }

    kPdpt[0]   = MakeTableEntry(kPdPhys, kWritable);
    kPdpt[510] = MakeTableEntry(kPdPhys, kWritable);
    kPml4[0]   = MakeTableEntry(kPdptPhys, kWritable);
    kPml4[511] = MakeTableEntry(kPdptPhys, kWritable);

    auto const&    frame = cinux::base::PtrAt<const cinux::boot::BootInfo>(
                               cinux::boot::LoadWord(cinux::boot::kHandoffMailboxInfo))
                               ->framebuffer;
    uint64_t const kFrameBytes =
        static_cast<uint64_t>(frame.pitch) * static_cast<uint64_t>(frame.height);
    auto const kDoors = PlanDeviceDoors(frame.physical, kFrameBytes);
    for (uint64_t door = 0; door < kDoors.count; ++door) {
        uint64_t const kIndex = kDoors.first_index + door;
        kPdpt[kIndex]         = MakeHugePageEntry(kIndex * kHugePageSize, kWritable);
    }
}
