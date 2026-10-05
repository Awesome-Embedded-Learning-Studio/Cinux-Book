#include "cinux/addr.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

using cinux::base::operator""_KiB;
using cinux::base::operator""_MiB;
using cinux::arch::page::Entry;
using cinux::arch::page::kPresent;
using cinux::mm::DirectMapVirt;
using cinux::mm::KernelTables;
using cinux::mm::walk::MapPage;
using cinux::mm::walk::TranslatePage;
using cinux::mm::walk::UnmapPage;

TEST("vmm: the user half of the live root is dead") {
    auto* const kPml4 = cinux::base::PtrAt<Entry>(DirectMapVirt(cinux::arch::ReadCr3()));
    for (unsigned long slot = 0; slot < cinux::mm::kDirectMapPml4; ++slot) {
        ASSERT_EQ(kPml4[slot].raw, 0ULL);
    }
}

TEST("vmm: the live root translates image and direct map") {
    KernelTables        tables;
    unsigned long const kRoot = cinux::arch::ReadCr3();
    ASSERT_EQ(TranslatePage(tables, kRoot, cinux::mm::kKernelImageBase + 2_MiB), 2_MiB);
    ASSERT_EQ(TranslatePage(tables, kRoot, DirectMapVirt(2_MiB)), 2_MiB);
    ASSERT_EQ(TranslatePage(tables, kRoot, cinux::mm::kHeapBase), 0UL);
}

TEST("vmm: a page lives in the heap window only while mapped") {
    KernelTables                tables;
    unsigned long const         kRoot = cinux::arch::ReadCr3();
    cinux::base::PhysAddr const kPage = cinux::mm::Pmm::self().allocate_page();
    ASSERT_TRUE(kPage != cinux::base::PhysAddr{});
    unsigned long const kSpot = cinux::mm::kHeapBase + 4_KiB;
    ASSERT_TRUE(MapPage(tables, kRoot, kSpot, kPage.raw));
    // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
    *cinux::base::PtrAt<unsigned long>(kSpot) = 0x114514UL;
    // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<unsigned long>(kSpot), 0x114514UL);
    ASSERT_EQ(TranslatePage(tables, kRoot, kSpot), kPage.raw);
    Entry const kGone = UnmapPage(tables, kRoot, kSpot);
    cinux::arch::InvalidatePage(kSpot);
    ASSERT_TRUE(kGone.has(kPresent));
    ASSERT_EQ(TranslatePage(tables, kRoot, kSpot), 0UL);
    cinux::mm::Pmm::self().free_page(kPage);
}

}  // namespace
