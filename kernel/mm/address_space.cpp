#include "kernel/mm/address_space.hpp"

#include "cinux/addr.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

namespace {

using cinux::arch::page::Entry;
using walk::MapPage;
using walk::TranslatePage;

constexpr unsigned long kKernelHalfSlots = 256;
constexpr unsigned long kSlotBytes       = sizeof(Entry);

}  // namespace

bool AddressSpace::init() {
    base::PhysAddr const kFrame = Pmm::self().allocate_page();
    if (kFrame == base::PhysAddr{}) {
        return false;
    }
    auto* const kFresh = cinux::base::PtrAt<Entry>(DirectMapVirt(kFrame.raw));
    cinux::base::SetBytes(kFresh, 0, cinux::arch::page::kSize);
    auto* const kKernel = cinux::base::PtrAt<Entry>(DirectMapVirt(KernelPageRoot()));
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

}  // namespace cinux::mm
