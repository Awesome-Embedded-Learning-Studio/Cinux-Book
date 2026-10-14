#include "cinux/addr.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

using cinux::base::operator""_KiB;
using cinux::arch::page::Entry;

TEST("aspace: a fresh space mirrors the kernel half and nothing else") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    const Entry* const kFresh = cinux::base::PtrAt<Entry>(cinux::mm::DirectMapVirt(space.root()));
    for (unsigned long slot = 0; slot < cinux::mm::kDirectMapPml4; ++slot) {
        ASSERT_EQ(kFresh[slot].raw, 0ULL);
    }
    const Entry* const kKernel =
        cinux::base::PtrAt<Entry>(cinux::mm::DirectMapVirt(cinux::mm::KernelPageRoot()));
    ASSERT_EQ(kFresh[cinux::mm::kDirectMapPml4].raw, kKernel[cinux::mm::kDirectMapPml4].raw);
    ASSERT_EQ(kFresh[cinux::mm::kKernelPml4].raw, kKernel[cinux::mm::kKernelPml4].raw);
}

TEST("aspace: pages map and translate inside the fresh space") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    unsigned long const kSpot = cinux::mm::kUserBrkMmapSplit + 4_KiB;
    ASSERT_EQ(space.translate(kSpot), 0UL);
    cinux::base::PhysAddr const kPage = cinux::mm::Pmm::self().allocate_page();
    ASSERT_TRUE(kPage != cinux::base::PhysAddr{});
    ASSERT_TRUE(space.map(kSpot, kPage.raw));
    ASSERT_EQ(space.translate(kSpot), kPage.raw);
    ASSERT_EQ(space.translate(cinux::mm::kUserNullTop + 4_KiB), 0UL);
}

TEST("aspace: activation switches the machine and comes home") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    unsigned long const         kSpot = cinux::mm::kUserBrkMmapSplit + 8_KiB;
    cinux::base::PhysAddr const kPage = cinux::mm::Pmm::self().allocate_page();
    ASSERT_TRUE(kPage != cinux::base::PhysAddr{});
    ASSERT_TRUE(space.map(kSpot, kPage.raw));
    space.activate();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *cinux::base::PtrAt<unsigned long>(kSpot) = 0x114514UL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<unsigned long>(kSpot),
              0x114514UL);  // NOLINT(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(cinux::arch::ReadCr3(), space.root());
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    ASSERT_EQ(cinux::arch::ReadCr3(), cinux::mm::KernelPageRoot());
}

TEST("aspace: teardown returns the whole user half to the frame allocator") {
    auto&                   ledger  = cinux::mm::Pmm::self();
    unsigned long const     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    unsigned long const kSpots[4] = {
        cinux::mm::kUserBrkMmapSplit + 16_KiB,
        cinux::mm::kUserBrkMmapSplit + 20_KiB,
        cinux::mm::kUserNullTop,
        cinux::mm::kUserNullTop + 8_KiB,
    };
    cinux::base::PhysAddr frames[4] = {};
    for (unsigned int index = 0; index < 4; ++index) {
        frames[index] = ledger.allocate_page();
        ASSERT_TRUE(frames[index] != cinux::base::PhysAddr{});
        ASSERT_TRUE(space.map_user(kSpots[index], frames[index].raw));
    }
    ASSERT_TRUE(ledger.free_page_count() < kBefore);
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
    ASSERT_EQ(space.root(), 0UL);
}

}  // namespace
