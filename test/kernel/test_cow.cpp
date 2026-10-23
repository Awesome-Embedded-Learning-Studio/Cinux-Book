#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

using cinux::base::operator""_KiB;

void arm_write_protect() {
    cinux::arch::WriteCr0(cinux::arch::ReadCr0() | cinux::arch::kCr0WriteProtect);
}

void map_shared(const cinux::mm::AddressSpace& space, unsigned long spot,
                const cinux::base::PhysAddr& frame) {
    cinux::base::safety::Check(space.map_user(spot, frame.raw), "cow test map failed");
    cinux::base::safety::Check(cinux::mm::cow::MarkPageShared(space.root(), spot),
                               "cow test mark failed");
}

/// The three terms one leaf's sharing state names, for the table below.
struct LeafWants {
    bool cow;
    bool writable;
    bool no_exec;
};

void assert_leaf_terms(unsigned long root, unsigned long spot, LeafWants wants) {
    cinux::mm::KernelTables       world;
    const cinux::mm::walk::SlotAt kSlot = cinux::mm::walk::FindSlot(world, root, spot);
    ASSERT_TRUE(kSlot.entry != nullptr);
    ASSERT_TRUE(kSlot.entry->has(cinux::arch::page::kCow) == wants.cow);
    ASSERT_TRUE(kSlot.entry->has(cinux::arch::page::kWritable) == wants.writable);
    ASSERT_TRUE(kSlot.entry->has(cinux::arch::page::kNoExec) == wants.no_exec);
}

TEST("cow: a shared write earns a private copy, company keeps the original") {
    cinux::mm::cow::InstallResolver();
    arm_write_protect();
    auto&                   ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    const cinux::base::PhysAddr kFrame = ledger.allocate_page();
    ASSERT_TRUE(kFrame != cinux::base::PhysAddr{});
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *cinux::base::PtrAt<unsigned long long>(cinux::mm::DirectMapVirt(kFrame.raw)) = 0x1111ULL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    const unsigned long kSpotA = cinux::mm::kUserNullTop + 64_KiB;
    const unsigned long kSpotB = cinux::mm::kUserNullTop + 128_KiB;
    map_shared(space, kSpotA, kFrame);
    map_shared(space, kSpotB, kFrame);
    space.activate();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *cinux::base::PtrAt<unsigned long long>(kSpotA) = 0xCAFEULL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<unsigned long long>(kSpotA), 0xCAFEULL);
    ASSERT_EQ(*cinux::base::PtrAt<unsigned long long>(kSpotB), 0x1111ULL);
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("cow: the no-execute bit survives sharing and the private copy") {
    cinux::mm::cow::InstallResolver();
    arm_write_protect();
    auto&                   ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    const cinux::base::PhysAddr kFrame = ledger.allocate_page();
    ASSERT_TRUE(kFrame != cinux::base::PhysAddr{});
    const unsigned long            kSpotA     = cinux::mm::kUserNullTop + 320_KiB;
    const unsigned long            kSpotB     = cinux::mm::kUserNullTop + 384_KiB;
    const cinux::arch::page::Entry kDataTerms = cinux::arch::page::UserLeafTerms(true, false);
    cinux::base::safety::Check(space.map_user(kSpotA, kFrame.raw, kDataTerms), "nx map failed");
    cinux::base::safety::Check(space.map_user(kSpotB, kFrame.raw, kDataTerms), "nx map failed");
    cinux::base::safety::Check(cinux::mm::cow::MarkPageShared(space.root(), kSpotA),
                               "nx mark failed");
    cinux::base::safety::Check(cinux::mm::cow::MarkPageShared(space.root(), kSpotB),
                               "nx mark failed");
    assert_leaf_terms(space.root(), kSpotA, {.cow = true, .writable = false, .no_exec = true});
    space.activate();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *cinux::base::PtrAt<unsigned long long>(kSpotA) = 0xD00DULL;
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    assert_leaf_terms(space.root(), kSpotA, {.cow = false, .writable = true, .no_exec = true});
    assert_leaf_terms(space.root(), kSpotB, {.cow = true, .writable = false, .no_exec = true});
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("cow: the last sharer writes in place, no frame changes hands") {
    cinux::mm::cow::InstallResolver();
    arm_write_protect();
    auto&                   ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    const cinux::base::PhysAddr kFrame = ledger.allocate_page();
    ASSERT_TRUE(kFrame != cinux::base::PhysAddr{});
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *cinux::base::PtrAt<unsigned long long>(cinux::mm::DirectMapVirt(kFrame.raw)) = 0x2222ULL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    const unsigned long kSpot = cinux::mm::kUserNullTop + 192_KiB;
    ASSERT_TRUE(space.map_user(kSpot, kFrame.raw));
    ASSERT_TRUE(cinux::mm::cow::MarkPageShared(space.root(), kSpot));
    const unsigned long kSeeded = ledger.free_page_count();
    space.activate();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *cinux::base::PtrAt<unsigned long long>(kSpot) = 0xBEEFULL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<unsigned long long>(kSpot), 0xBEEFULL);
    ASSERT_EQ(ledger.free_page_count(), kSeeded);
    ASSERT_EQ(space.translate(kSpot), kFrame.raw);
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("cow: faults the engine cannot answer pass the question on") {
    const unsigned long      kUnmapped = cinux::mm::kUserNullTop + 256_KiB;
    const unsigned long long kUserWrite =
        cinux::arch::isr::kFaultWasWrite | cinux::arch::isr::kFaultFromUser;
    ASSERT_TRUE(!cinux::mm::cow::TryResolvePageFault(kUnmapped, kUserWrite));
    ASSERT_TRUE(!cinux::mm::cow::TryResolvePageFault(kUnmapped, 0));
    ASSERT_TRUE(!cinux::mm::cow::TryResolvePageFault(kUnmapped, cinux::arch::isr::kFaultWasWrite));
}

}  // namespace
