#include "kernel/mm/cow.hpp"

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "cinux/spin_mutex.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/isr.hpp"  // NOLINT(misc-include-cleaner) fault-code words shared with the reporter
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm::cow {

namespace {

using cinux::arch::page::Entry;
using walk::FindSlot;
using walk::SlotAt;

base::SpinMutex g_fault_gate;

unsigned long long release_share(unsigned long long frame) {
    return Pmm::self().release_frame(cinux::base::PhysAddr{frame});
}

Entry shared_flags() {
    return cinux::arch::page::kUser | cinux::arch::page::kCow;
}

Entry private_flags(bool writable) {
    Entry flags = cinux::arch::page::kUser;
    if (writable) {
        flags = flags | cinux::arch::page::kWritable;
    }
    return flags;
}

[[nodiscard]] walk::WalkLevel level_below(walk::WalkLevel level) {
    return static_cast<walk::WalkLevel>(static_cast<unsigned char>(level) + 1);
}

struct CloneWalk {
    AddressSpace* child;
    unsigned long parent_root;
};

void share_leaf(Entry& slot, unsigned long spot, const CloneWalk& context) {
    const unsigned long long kTarget = walk::EntryTarget(slot);
    cinux::base::safety::Check(context.child->map_user(spot, kTarget), "fork clone refused a map");
    cinux::base::safety::Check(MarkPageShared(context.parent_root, spot),
                               "fork clone lost a parent page");
    cinux::base::safety::Check(MarkPageShared(context.child->root(), spot),
                               "fork clone lost a child page");
}

void descend(Entry* table, walk::WalkLevel level, unsigned long base, const CloneWalk& context);

void share_entry(Entry& slot, walk::WalkLevel level, unsigned long spot, const CloneWalk& context) {
    if (level == walk::WalkLevel::kPageTable) {
        share_leaf(slot, spot, context);
        return;
    }
    cinux::base::safety::Check(!slot.has(cinux::arch::page::kLarge),
                               "user half holds a large page");
    const unsigned long long kTarget = walk::EntryTarget(slot);
    descend(cinux::base::PtrAt<Entry>(cinux::mm::DirectMapVirt(kTarget)), level_below(level), spot,
            context);
}

void descend(Entry* table, walk::WalkLevel level, unsigned long base, const CloneWalk& context) {
    const unsigned char kShift = walk::IndexRange(level).low;
    for (unsigned long index = 0; index < walk::kEntriesPerTable; ++index) {
        Entry& slot = table[index];
        if (slot.has(cinux::arch::page::kPresent)) {
            share_entry(slot, level, base | (index << kShift), context);
        }
    }
}

}  // namespace

bool MarkPageShared(unsigned long root, unsigned long virtual_address) {
    const arch::IrqGuard    kIrq;
    const auto              kGate = g_fault_gate.guard();
    cinux::mm::KernelTables world;
    const SlotAt            kSlot = FindSlot(world, root, virtual_address);
    if (kSlot.entry == nullptr || kSlot.level != walk::WalkLevel::kPageTable) {
        return false;
    }
    const Entry kLeaf = *kSlot.entry;
    if (!kLeaf.has(cinux::arch::page::kPresent)) {
        return false;
    }
    if (kLeaf.has(cinux::arch::page::kCow) && !kLeaf.has(cinux::arch::page::kWritable)) {
        return true;
    }
    const unsigned long long kTarget = walk::EntryTarget(kLeaf);
    Pmm::self().retain_frame(cinux::base::PhysAddr{kTarget});
    *kSlot.entry = cinux::arch::page::MakeTableEntry(kTarget, shared_flags());
    cinux::arch::InvalidatePage(virtual_address);
    return true;
}

bool TryResolvePageFault(unsigned long long address, unsigned long long error_code) {
    const arch::IrqGuard kIrq;
    const auto           kGate = g_fault_gate.guard();
    if ((error_code & cinux::arch::isr::kFaultWasWrite) == 0) {
        return false;
    }
    cinux::mm::KernelTables world;
    const SlotAt            kSlot = FindSlot(world, cinux::arch::ReadCr3(), address);
    if (kSlot.entry == nullptr || kSlot.level != walk::WalkLevel::kPageTable) {
        return false;
    }
    const Entry kLeaf = *kSlot.entry;
    if (!kLeaf.has(cinux::arch::page::kPresent) || kLeaf.has(cinux::arch::page::kWritable) ||
        !kLeaf.has(cinux::arch::page::kCow)) {
        return false;
    }
    const unsigned long long kTarget  = walk::EntryTarget(kLeaf);
    const unsigned long long kCompany = release_share(kTarget);
    if (kCompany == 0) {
        *kSlot.entry = cinux::arch::page::MakeTableEntry(kTarget, private_flags(true));
        cinux::arch::InvalidatePage(address);
        return true;
    }
    const cinux::base::PhysAddr kCopy = cinux::mm::Pmm::self().allocate_page();
    cinux::base::safety::Check(kCopy != cinux::base::PhysAddr{}, "cow copy out of frames");
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const       kDestination = reinterpret_cast<void*>(cinux::mm::DirectMapVirt(kCopy.raw));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto* const kSource = reinterpret_cast<const void*>(cinux::mm::DirectMapVirt(kTarget));
    cinux::base::CopyBytes(kDestination, kSource, cinux::arch::page::kSize);
    *kSlot.entry = cinux::arch::page::MakeTableEntry(kCopy.raw, private_flags(true));
    cinux::arch::InvalidatePage(address);
    return true;
}

void InstallResolver() {
    cinux::arch::isr::InstallPageFaultResolver(&TryResolvePageFault);
}

void ReleaseFrame(unsigned long long frame) {
    const arch::IrqGuard kIrq;
    const auto           kGate = g_fault_gate.guard();
    if (release_share(frame) == 0) {
        cinux::mm::Pmm::self().free_page(cinux::base::PhysAddr{frame});
    }
}

AddressSpace* CloneSharedSpace(const AddressSpace& parent) {
    const unsigned long kParentRoot = parent.root();
    if (kParentRoot == 0) {
        return nullptr;
    }
    auto* const kChild = new AddressSpace{};
    if (kChild == nullptr || !kChild->init()) {
        delete kChild;
        return nullptr;
    }
    // NOLINTNEXTLINE(misc-const-correctness) the walk rewrites leaf entries
    auto* const     kRoot = cinux::base::PtrAt<Entry>(cinux::mm::DirectMapVirt(kParentRoot));
    const CloneWalk kContext{.child = kChild, .parent_root = kParentRoot};
    for (unsigned long slot = 0; slot < walk::kUserHalfSlots; ++slot) {
        Entry& leaf = kRoot[slot];
        if (!leaf.has(cinux::arch::page::kPresent)) {
            continue;
        }
        cinux::base::safety::Check(!leaf.has(cinux::arch::page::kLarge),
                                   "user half holds a large page");
        const unsigned long long kTarget = walk::EntryTarget(leaf);
        const unsigned long      kBase   = slot << walk::IndexRange(walk::WalkLevel::kPml4).low;
        descend(cinux::base::PtrAt<Entry>(cinux::mm::DirectMapVirt(kTarget)),
                walk::WalkLevel::kPdpt, kBase, kContext);
    }
    if (arch::ReadCr3() == kParentRoot) {
        arch::LoadCr3(kParentRoot);
    }
    return kChild;
}

}  // namespace cinux::mm::cow
