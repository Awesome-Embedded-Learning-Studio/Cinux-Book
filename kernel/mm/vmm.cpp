#include "kernel/mm/vmm.hpp"

#include <cstdint>

#include "cinux/addr.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/pmm_config.hpp"
#include "kernel/mm/vmm_config.hpp"

namespace cinux::mm {

namespace {

using cinux::arch::page::Entry;
using cinux::arch::page::HugeWindowBase;
using cinux::arch::page::LargePageBase;
using cinux::arch::page::MakeLargePageEntry;
using cinux::arch::page::MakeTableEntry;
using cinux::arch::page::kHugePageSize;
using cinux::arch::page::kLargePageSize;
using cinux::arch::page::kWritable;
using walk::EntryTarget;
using walk::SlotIndex;
using walk::WalkLevel;

constexpr unsigned int kEntriesPerTable = 512;

class SeedPantry {
public:
    static SeedPantry& self() {
        static SeedPantry local_pantry;
        return local_pantry;
    }

    unsigned long take_page() {
        if (cursor_ + cinux::arch::page::kSize > kSeedTablesSpan.top) {
            return 0;
        }
        unsigned long const kPhysical = cursor_;
        cursor_ += cinux::arch::page::kSize;
        cinux::base::SetBytes(cinux::base::PtrAt<void>(kPhysical), 0,
                              sizeof(Entry) * kEntriesPerTable);
        return kPhysical;
    }

private:
    unsigned long cursor_ = kSeedTablesSpan.base;
};

Entry* table_at(unsigned long physical) {
    return cinux::base::PtrAt<Entry>(physical);
}

Entry* direct_table_at(unsigned long physical) {
    return cinux::base::PtrAt<Entry>(DirectMapVirt(physical));
}

bool window_touches_usable(const cinux::boot::BootInfo& info, unsigned long window) {
    unsigned long const kWindowTop = window + kHugePageSize;
    for (uint32_t index = 0; index < info.e820_count; ++index) {
        if (info.e820[index].type != cinux::boot::kE820Usable) {
            continue;
        }
        auto const kBase = static_cast<unsigned long>(info.e820[index].base);
        auto const kTop  = kBase + static_cast<unsigned long>(info.e820[index].length);
        if (kBase < kWindowTop && window < kTop) {
            return true;
        }
    }
    return false;
}

void fill_usable_ram_pages(Entry* page_directory, unsigned long window,
                           const cinux::boot::BootInfo& info) {
    unsigned long const kWindowTop = window + kHugePageSize;
    for (uint32_t index = 0; index < info.e820_count; ++index) {
        if (info.e820[index].type != cinux::boot::kE820Usable) {
            continue;
        }
        auto const          kBase = static_cast<unsigned long>(info.e820[index].base);
        auto const          kTop  = kBase + static_cast<unsigned long>(info.e820[index].length);
        unsigned long const kLo   = kBase > window ? kBase : window;
        unsigned long const kHi   = kTop < kWindowTop ? kTop : kWindowTop;
        if (kLo >= kHi) {
            continue;
        }
        for (unsigned long base = LargePageBase(kLo); base < kHi; base += kLargePageSize) {
            page_directory[SlotIndex(base, WalkLevel::kPageDirectory)] =
                MakeLargePageEntry(base, kWritable);
        }
    }
}

unsigned long highest_usable_top(const cinux::boot::BootInfo& info) {
    unsigned long top = 0;
    for (uint32_t index = 0; index < info.e820_count; ++index) {
        if (info.e820[index].type != cinux::boot::kE820Usable) {
            continue;
        }
        auto const          kEntryTop = static_cast<unsigned long>(info.e820[index].base) +
                                        static_cast<unsigned long>(info.e820[index].length);
        unsigned long const kClamped  = kEntryTop < kPmmMaxPhys ? kEntryTop : kPmmMaxPhys;
        top                           = kClamped > top ? kClamped : top;
    }
    return top;
}

class HandoffArchive {
public:
    static HandoffArchive& self() {
        static HandoffArchive local_archive;
        return local_archive;
    }

    void store(const cinux::boot::BootInfo& handoff) {
        cinux::base::CopyBytes(&record_, &handoff, sizeof(cinux::boot::BootInfo));
    }

    [[nodiscard]] const cinux::boot::BootInfo& record() const { return record_; }

private:
    alignas(16) cinux::boot::BootInfo record_ = {};
};

class KernelRoot {
public:
    static KernelRoot& self() {
        static KernelRoot local_root;
        return local_root;
    }

    void capture(unsigned long root) { value_ = root; }

    [[nodiscard]] unsigned long value() const { return value_; }

private:
    unsigned long value_ = 0;
};

}  // namespace

Entry* KernelTables::table(unsigned long physical) {
    return cinux::base::PtrAt<Entry>(DirectMapVirt(physical));
}

unsigned long KernelTables::take_page() {
    base::PhysAddr const kFresh = Pmm::self().allocate_page();
    if (kFresh == base::PhysAddr{}) {
        return 0;
    }
    cinux::base::SetBytes(cinux::base::PtrAt<void>(DirectMapVirt(kFresh.raw)), 0,
                          sizeof(Entry) * kEntriesPerTable);
    return kFresh.raw;
}

void MapFramebufferDoor(const cinux::boot::BootInfo& info) {
    unsigned long const kFrameBytes = static_cast<unsigned long>(info.framebuffer.pitch) *
                                      static_cast<unsigned long>(info.framebuffer.height);
    if (kFrameBytes == 0 || info.framebuffer.physical == 0) {
        return;
    }
    Entry* const        kPml4     = table_at(cinux::arch::ReadCr3());
    unsigned long const kPdptPhys = SeedPantry::self().take_page();
    if (kPdptPhys == 0) {
        return;
    }
    Entry* const        kPdpt    = table_at(kPdptPhys);
    unsigned long const kSpanTop = info.framebuffer.physical + kFrameBytes;
    for (unsigned long window = HugeWindowBase(info.framebuffer.physical); window < kSpanTop;
         window += kHugePageSize) {
        unsigned long const kDirectoryPhys = SeedPantry::self().take_page();
        if (kDirectoryPhys == 0) {
            return;
        }
        Entry* const        kDirectory = table_at(kDirectoryPhys);
        unsigned long const kLo =
            info.framebuffer.physical > window ? info.framebuffer.physical : window;
        unsigned long const kHi =
            kSpanTop < window + kHugePageSize ? kSpanTop : window + kHugePageSize;
        for (unsigned long base = LargePageBase(kLo); base < kHi; base += kLargePageSize) {
            kDirectory[SlotIndex(base, WalkLevel::kPageDirectory)] =
                MakeLargePageEntry(base, kWritable);
        }
        kPdpt[SlotIndex(window, WalkLevel::kPdpt)] = MakeTableEntry(kDirectoryPhys, kWritable);
    }
    kPml4[kIoremapPml4] = MakeTableEntry(kPdptPhys, kWritable);
}

const cinux::boot::BootInfo* BringUpAddressSpace(const cinux::boot::BootInfo& handoff) {
    HandoffArchive::self().store(handoff);
    Entry* const kPml4 = table_at(cinux::arch::ReadCr3());

    unsigned long const kPdptPhys = SeedPantry::self().take_page();
    if (kPdptPhys == 0) {
        return nullptr;
    }
    unsigned long const kRamTop = highest_usable_top(HandoffArchive::self().record());
    for (unsigned long window = 0; window < kRamTop; window += kHugePageSize) {
        if (!window_touches_usable(HandoffArchive::self().record(), window)) {
            continue;
        }
        unsigned long const kDirectoryPhys = SeedPantry::self().take_page();
        if (kDirectoryPhys == 0) {
            return nullptr;
        }
        fill_usable_ram_pages(table_at(kDirectoryPhys), window, HandoffArchive::self().record());
        table_at(kPdptPhys)[SlotIndex(window, WalkLevel::kPdpt)] =
            MakeTableEntry(kDirectoryPhys, kWritable);
    }
    kPml4[kDirectMapPml4] = MakeTableEntry(kPdptPhys, kWritable);

    MapFramebufferDoor(HandoffArchive::self().record());

    Entry* const kSharedPdpt = direct_table_at(EntryTarget(kPml4[kKernelPml4]));
    for (unsigned int i = 0; i < kEntriesPerTable; ++i) {
        if (i != kKernelImagePdpt) {
            kSharedPdpt[i] = Entry{};
        }
    }

    direct_table_at(cinux::arch::ReadCr3())[0] = Entry{};
    cinux::arch::ReloadCr3();
    KernelRoot::self().capture(cinux::arch::ReadCr3());
    return &HandoffArchive::self().record();
}

unsigned long KernelPageRoot() {
    return KernelRoot::self().value();
}

}  // namespace cinux::mm
