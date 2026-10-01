#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "lm.hpp"

namespace {
using cinux::arch::page::Entry;
using cinux::arch::page::kLargePageSize;
using cinux::arch::page::kWritable;
using cinux::arch::page::MakeLargePageEntry;
using cinux::arch::page::MakeTableEntry;

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

    for (unsigned int i = 0; i < kEntriesPerPage; ++i) {
        kPml4[i] = Entry{};
        kPdpt[i] = Entry{};
        kPd[i]   = Entry{};
    }

    unsigned long const kCovered   = g_kernel_end_paddr + 0x1FFFFFUL;
    auto const          kDoorCount = static_cast<unsigned int>(kCovered >> 21);
    for (unsigned int i = 0; i < kDoorCount; ++i) {
        kPd[i] = MakeLargePageEntry(i * kLargePageSize, kWritable);
    }

    kPdpt[0]   = MakeTableEntry(kPdPhys, kWritable);
    kPdpt[510] = MakeTableEntry(kPdPhys, kWritable);
    kPml4[0]   = MakeTableEntry(kPdptPhys, kWritable);
    kPml4[511] = MakeTableEntry(kPdptPhys, kWritable);
}
