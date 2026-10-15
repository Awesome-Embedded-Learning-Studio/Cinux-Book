#include "kernel/mm/address_space.hpp"

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

namespace {

using cinux::arch::page::Entry;
using walk::MapPage;
using walk::MapUserPage;
using walk::TranslatePage;

constexpr unsigned long kKernelHalfSlots = 256;
constexpr unsigned long kSlotBytes       = sizeof(Entry);

constexpr int kPdptLevelsBelow = 2;
constexpr int kLeafTable       = 0;

void release_user_tables(Entry* table, int levels_below) {
    for (unsigned long index = 0; index < walk::kEntriesPerTable; ++index) {
        Entry& slot = table[index];
        if (!slot.has(cinux::arch::page::kPresent)) {
            continue;
        }
        unsigned long const kTarget = walk::EntryTarget(slot);
        if (levels_below == kLeafTable) {
            cow::ReleaseFrame(kTarget);
            continue;
        }
        cinux::base::safety::Check(!slot.has(cinux::arch::page::kLarge),
                                   "user half holds a large page");
        release_user_tables(cinux::base::PtrAt<Entry>(DirectMapVirt(kTarget)), levels_below - 1);
        Pmm::self().free_page(cinux::base::PhysAddr{kTarget});
    }
}

}  // namespace

bool AddressSpace::init() {
    base::PhysAddr const kFrame = Pmm::self().allocate_page();
    if (kFrame == base::PhysAddr{}) {
        return false;
    }
    auto* const kFresh = cinux::base::PtrAt<Entry>(DirectMapVirt(kFrame.raw));
    cinux::base::SetBytes(kFresh, 0, cinux::arch::page::kSize);
    const Entry* const kKernel = cinux::base::PtrAt<Entry>(DirectMapVirt(KernelPageRoot()));
    cinux::base::CopyBytes(kFresh + kKernelHalfSlots, kKernel + kKernelHalfSlots,
                           kKernelHalfSlots * kSlotBytes);
    root_ = kFrame.raw;
    return true;
}

unsigned long AddressSpace::root() const {
    return root_;
}

bool AddressSpace::map(unsigned long virtual_address, unsigned long physical) const {
    if (root_ == 0) {
        return false;
    }
    KernelTables tables;
    return MapPage(tables, root_, virtual_address, physical);
}

bool AddressSpace::map_user(unsigned long virtual_address, unsigned long physical,
                            cinux::arch::page::Entry leaf_bits) const {
    if (root_ == 0) {
        return false;
    }
    KernelTables tables;
    return MapUserPage(tables, root_, virtual_address,
                       cinux::arch::page::MakeTableEntry(physical, leaf_bits));
}

unsigned long AddressSpace::translate(unsigned long virtual_address) const {
    if (root_ == 0) {
        return 0;
    }
    KernelTables tables;
    return TranslatePage(tables, root_, virtual_address);
}

void AddressSpace::activate() const {
    if (root_ != 0) {
        cinux::arch::LoadCr3(root_);
    }
}

void AddressSpace::teardown() {
    if (root_ == 0) {
        return;
    }
    // NOLINTNEXTLINE(misc-const-correctness) the walk rewrites leaf entries
    auto* const kRoot = cinux::base::PtrAt<Entry>(DirectMapVirt(root_));
    for (unsigned long index = 0; index < kKernelHalfSlots; ++index) {
        Entry& slot = kRoot[index];
        if (!slot.has(cinux::arch::page::kPresent)) {
            continue;
        }
        cinux::base::safety::Check(!slot.has(cinux::arch::page::kLarge),
                                   "user half holds a large page");
        unsigned long const kTarget = walk::EntryTarget(slot);
        release_user_tables(cinux::base::PtrAt<Entry>(DirectMapVirt(kTarget)), kPdptLevelsBelow);
        Pmm::self().free_page(cinux::base::PhysAddr{kTarget});
    }
    Pmm::self().free_page(cinux::base::PhysAddr{root_});
    root_ = 0;
}

}  // namespace cinux::mm
