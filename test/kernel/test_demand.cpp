#include "cinux/addr.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/fs/ramfs/ramfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/demand.hpp"
#include "kernel/mm/hardware_frames.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

using cinux::base::operator""_KiB;
using cinux::base::operator""_MiB;

constexpr unsigned long long kUserRead = cinux::arch::isr::kFaultFromUser;

constexpr unsigned long long kUserWrite =
    cinux::arch::isr::kFaultFromUser | cinux::arch::isr::kFaultWasWrite;

constexpr unsigned long long kKernelWrite = cinux::arch::isr::kFaultWasWrite;

/// One anonymous promise the tests register and fill.
struct RegionPlan {
    unsigned long           start    = 0;
    unsigned long           bytes    = 0;
    bool                    writable = false;
    cinux::mm::VmaKind      kind     = cinux::mm::VmaKind::kAnonymous;
    cinux::fs::FsNode       backing{};
    const cinux::fs::FsOps* file_ops    = nullptr;
    void*                   file_self   = nullptr;
    unsigned long long      file_offset = 0;
};

cinux::mm::VmaRegion make_region(const RegionPlan& plan) {
    cinux::mm::VmaRegion region{};
    region.start       = plan.start;
    region.end         = plan.start + plan.bytes;
    region.kind        = plan.kind;
    region.readable    = true;
    region.writable    = plan.writable;
    region.backing     = plan.backing;
    region.file_ops    = plan.file_ops;
    region.file_self   = plan.file_self;
    region.file_offset = plan.file_offset;
    return region;
}

TEST("demand: an anonymous write fault earns one zeroed page") {
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 64_KiB;
    ASSERT_TRUE(
        store.register_range(make_region({kSpot, 16_KiB, true, cinux::mm::VmaKind::kAnonymous}))
            .ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserWrite));
    const unsigned long kFrame = space.translate(kSpot);
    ASSERT_TRUE(kFrame != 0);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<unsigned long long>(cinux::mm::DirectMapVirt(kFrame)), 0ULL);
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("demand: a filled data leaf carries the no-execute bit") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 192_KiB;
    ASSERT_TRUE(store
                    .register_range(make_region({.start    = kSpot,
                                                 .bytes    = 16_KiB,
                                                 .writable = true,
                                                 .kind     = cinux::mm::VmaKind::kAnonymous}))
                    .ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserWrite));
    cinux::mm::KernelTables       world;
    const cinux::mm::walk::SlotAt kSlot = cinux::mm::walk::FindSlot(world, space.root(), kSpot);
    ASSERT_TRUE(kSlot.entry != nullptr);
    ASSERT_TRUE(kSlot.entry->has(cinux::arch::page::kNoExec));
    ASSERT_TRUE(kSlot.entry->has(cinux::arch::page::kWritable));
    space.teardown();
}

TEST("demand: a filled code leaf keeps its execute promise") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore  store;
    const unsigned long  kSpot = cinux::mm::kUserNullTop + 256_KiB;
    cinux::mm::VmaRegion region =
        make_region({.start = kSpot, .bytes = 16_KiB, .kind = cinux::mm::VmaKind::kAnonymous});
    region.executable = true;
    ASSERT_TRUE(store.register_range(region).ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserRead));
    cinux::mm::KernelTables       world;
    const cinux::mm::walk::SlotAt kSlot = cinux::mm::walk::FindSlot(world, space.root(), kSpot);
    ASSERT_TRUE(kSlot.entry != nullptr);
    ASSERT_FALSE(kSlot.entry->has(cinux::arch::page::kNoExec));
    ASSERT_FALSE(kSlot.entry->has(cinux::arch::page::kWritable));
    space.teardown();
}

TEST("demand: a kernel-origin write on a promised page fills anyway") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 64_KiB;
    ASSERT_TRUE(
        store.register_range(make_region({kSpot, 16_KiB, true, cinux::mm::VmaKind::kAnonymous}))
            .ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kKernelWrite));
    ASSERT_TRUE(space.translate(kSpot) != 0);
    space.teardown();
}

TEST("demand: a kernel-origin write with no promise still declines") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    const cinux::mm::VmaStore kStore;
    const unsigned long       kSpot = cinux::mm::kUserNullTop + 64_KiB;
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, kStore, kSpot, kKernelWrite));
    space.teardown();
}

TEST("demand: a present page and a foreign promise decline") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 64_KiB;
    ASSERT_TRUE(
        store.register_range(make_region({kSpot, 16_KiB, true, cinux::mm::VmaKind::kAnonymous}))
            .ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserWrite));
    const unsigned long kFirst = space.translate(kSpot);
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, store, kSpot, kUserWrite));
    ASSERT_EQ(space.translate(kSpot), kFirst);
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot + 4_KiB, kUserRead));
    ASSERT_TRUE(space.translate(kSpot + 4_KiB) != kFirst);
    space.teardown();
}

TEST("demand: a read-only promise fills without the write bit") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 128_KiB;
    ASSERT_TRUE(store
                    .register_range(make_region(
                        {kSpot, cinux::arch::page::kSize, false, cinux::mm::VmaKind::kAnonymous}))
                    .ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserRead));
    cinux::mm::KernelTables       world;
    const cinux::mm::walk::SlotAt kSlot = cinux::mm::walk::FindSlot(world, space.root(), kSpot);
    ASSERT_TRUE(kSlot.entry != nullptr);
    ASSERT_TRUE(kSlot.entry->has(cinux::arch::page::kPresent));
    ASSERT_TRUE(!kSlot.entry->has(cinux::arch::page::kWritable));
    space.teardown();
}

TEST("demand: the heap span fills, past its end declines") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    store.seed_heap(cinux::mm::kUserNullTop + 1_MiB, cinux::mm::kUserNullTop + 2_MiB);
    const unsigned long kBase = store.heap_end();
    ASSERT_TRUE(store.move_heap_end(kBase + cinux::arch::page::kSize).ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kBase, kUserWrite));
    ASSERT_TRUE(space.translate(kBase) != 0);
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, store, kBase + 1_MiB, kUserWrite));
    space.teardown();
}

TEST("demand: a file-backed fault reads through the backend") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore                          store;
    cinux::fs::RamFs                             volume;
    static constexpr cinux::fs::FsOps            kOps  = cinux::fs::EraseFs<cinux::fs::RamFs>();
    const cinux::base::Result<cinux::fs::FsNode> kFile = volume.make_file("notes");
    ASSERT_TRUE(kFile.ok());
    unsigned char pattern[cinux::arch::page::kSize];
    for (unsigned long index = 0; index < sizeof(pattern); ++index) {
        pattern[index] = static_cast<unsigned char>(index);
    }
    ASSERT_TRUE(volume.write(kFile.value(), 0, pattern, sizeof(pattern)).ok());
    const unsigned long kSpot = cinux::mm::kUserNullTop + 256_KiB;
    const RegionPlan    kPlan{
        .start     = kSpot,
        .bytes     = cinux::arch::page::kSize,
        .writable  = false,
        .kind      = cinux::mm::VmaKind::kFile,
        .backing   = kFile.value(),
        .file_ops  = &kOps,
        .file_self = &volume,
    };
    ASSERT_TRUE(store.register_range(make_region(kPlan)).ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserRead));
    const unsigned long kFrame = space.translate(kSpot);
    ASSERT_TRUE(kFrame != 0);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    const auto* const kPage =
        cinux::base::PtrAt<const unsigned char>(cinux::mm::DirectMapVirt(kFrame));
    ASSERT_TRUE(kPage[0] == pattern[0]);
    ASSERT_TRUE(kPage[4095] == pattern[4095]);
    space.teardown();
}

TEST("demand: a file tail past the bytes stays zero") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore                          store;
    cinux::fs::RamFs                             volume;
    static constexpr cinux::fs::FsOps            kOps  = cinux::fs::EraseFs<cinux::fs::RamFs>();
    const cinux::base::Result<cinux::fs::FsNode> kFile = volume.make_file("notes");
    ASSERT_TRUE(kFile.ok());
    ASSERT_TRUE(volume.write(kFile.value(), 0, "tail", 5).ok());
    const unsigned long kSpot = cinux::mm::kUserNullTop + 256_KiB;
    const RegionPlan    kPlan{
        .start     = kSpot,
        .bytes     = 2 * cinux::arch::page::kSize,
        .writable  = false,
        .kind      = cinux::mm::VmaKind::kFile,
        .backing   = kFile.value(),
        .file_ops  = &kOps,
        .file_self = &volume,
    };
    ASSERT_TRUE(store.register_range(make_region(kPlan)).ok());
    const unsigned long kTailSpot = kSpot + cinux::arch::page::kSize;
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kTailSpot, kUserRead));
    const unsigned long kTail = space.translate(kTailSpot);
    ASSERT_TRUE(kTail != 0);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    const auto* const kTailPage =
        cinux::base::PtrAt<const unsigned char>(cinux::mm::DirectMapVirt(kTail));
    ASSERT_TRUE(kTailPage[0] == 0);
    space.teardown();
}

TEST("demand: a file promise without a backend face declines") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore                          store;
    cinux::fs::RamFs                             volume;
    const cinux::base::Result<cinux::fs::FsNode> kFile = volume.make_file("notes");
    ASSERT_TRUE(kFile.ok());
    const unsigned long kSpot = cinux::mm::kUserNullTop + 256_KiB;
    const RegionPlan    kFaceless{
        .start    = kSpot,
        .bytes    = cinux::arch::page::kSize,
        .writable = false,
        .kind     = cinux::mm::VmaKind::kFile,
        .backing  = kFile.value(),
    };
    ASSERT_TRUE(store.register_range(make_region(kFaceless)).ok());
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, store, kFaceless.start, kUserRead));
    space.teardown();
}

TEST("demand: the stack fills downward toward its guard") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const RegionPlan    kPlan{
        .start    = cinux::mm::kUserStackBottom,
        .bytes    = cinux::mm::kUserStackTop - cinux::mm::kUserStackBottom,
        .writable = true,
        .kind     = cinux::mm::VmaKind::kStack,
    };
    ASSERT_TRUE(store.register_range(make_region(kPlan)).ok());
    const unsigned long kTopPage = cinux::mm::kUserStackTop - cinux::arch::page::kSize;
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kTopPage, kUserWrite));
    ASSERT_TRUE(space.translate(kTopPage) != 0);
    const unsigned long kLower = cinux::mm::kUserStackBottom + cinux::arch::page::kSize;
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kLower, kUserWrite));
    ASSERT_TRUE(space.translate(kLower) != 0);
    space.teardown();
}

TEST("demand: the guard page and the sky above the stack decline") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const RegionPlan    kPlan{
        .start    = cinux::mm::kUserStackBottom,
        .bytes    = cinux::mm::kUserStackTop - cinux::mm::kUserStackBottom,
        .writable = true,
        .kind     = cinux::mm::VmaKind::kStack,
    };
    ASSERT_TRUE(store.register_range(make_region(kPlan)).ok());
    ASSERT_TRUE(
        !cinux::mm::demand::FillPage(space, store, cinux::mm::kUserStackBottom, kUserWrite));
    ASSERT_TRUE(space.translate(cinux::mm::kUserStackBottom) == 0);
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, store, cinux::mm::kUserStackTop, kUserWrite));
    space.teardown();
}

TEST("demand: faults nobody promised are declined, kernel-side included") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    const cinux::mm::VmaStore kEmptyStore;
    const unsigned long       kOutside = cinux::mm::kUserNullTop + 384_KiB;
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, kEmptyStore, kOutside, kUserWrite));
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, kEmptyStore, kOutside,
                                             cinux::arch::isr::kFaultWasWrite));
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, kEmptyStore, kOutside,
                                             kUserWrite | cinux::arch::isr::kFaultProtection));
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, kEmptyStore, 0x100000000000ULL, kUserWrite));
    space.teardown();
}


/// The fake memory device: one reserved hardware frame behind one page.
unsigned long g_device_frame = 0;

cinux::base::Result<unsigned long> fake_frame_at(void* /*self*/, cinux::fs::FsNode /*node*/,
                                                 unsigned long long offset) {
    if (offset >= cinux::arch::page::kSize) {
        return cinux::base::KernelError::kInvalidArgument;
    }
    return g_device_frame;
}

const cinux::fs::FsOps kDeviceOps{
    .frame_at = fake_frame_at,
};

TEST("demand: a device fault maps the named hardware frame") {
    auto&                       ledger  = cinux::mm::Pmm::self();
    const unsigned long         kBefore = ledger.free_page_count();
    const cinux::base::PhysAddr kFrame  = ledger.allocate_page();
    ASSERT_TRUE(kFrame != cinux::base::PhysAddr{});
    ASSERT_TRUE(cinux::mm::ReserveHardwarePages(kFrame.raw, 1));
    g_device_frame = kFrame.raw;
    auto* const kMarker =
        cinux::base::PtrAt<unsigned long long>(cinux::mm::DirectMapVirt(kFrame.raw));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *kMarker = 0x484172444547414DULL;
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 320_KiB;
    const RegionPlan    kPlan{
        .start     = kSpot,
        .bytes     = cinux::arch::page::kSize,
        .writable  = true,
        .kind      = cinux::mm::VmaKind::kDevice,
        .backing   = cinux::fs::FsNode{},
        .file_ops  = &kDeviceOps,
        .file_self = nullptr,
    };
    ASSERT_TRUE(store.register_range(make_region(kPlan)).ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserWrite));
    ASSERT_EQ(space.translate(kSpot), kFrame.raw);
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore - 1);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    *kMarker       = 0;
    g_device_frame = 0;
}

TEST("demand: a device offset outside the span declines") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore         store;
    const cinux::base::PhysAddr kFrame = cinux::mm::Pmm::self().allocate_page();
    ASSERT_TRUE(kFrame != cinux::base::PhysAddr{});
    g_device_frame            = kFrame.raw;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 320_KiB;
    const RegionPlan    kPlan{
        .start    = kSpot,
        .bytes    = 4 * cinux::arch::page::kSize,
        .writable = true,
        .kind     = cinux::mm::VmaKind::kDevice,
        .file_ops = &kDeviceOps,
    };
    ASSERT_TRUE(store.register_range(make_region(kPlan)).ok());
    ASSERT_TRUE(cinux::mm::demand::FillPage(space, store, kSpot, kUserWrite));
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, store, kSpot + (2 * cinux::arch::page::kSize),
                                             kUserWrite));
    space.teardown();
    g_device_frame = 0;
}

TEST("demand: a device promise without a frame face declines") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore store;
    const unsigned long kSpot = cinux::mm::kUserNullTop + 320_KiB;
    const RegionPlan    kPlan{
        .start    = kSpot,
        .bytes    = cinux::arch::page::kSize,
        .writable = true,
        .kind     = cinux::mm::VmaKind::kDevice,
    };
    ASSERT_TRUE(store.register_range(make_region(kPlan)).ok());
    ASSERT_TRUE(!cinux::mm::demand::FillPage(space, store, kSpot, kUserWrite));
    space.teardown();
}

}  // namespace
