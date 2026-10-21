#include "cinux/addr.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/driver/apic/io_apic.hpp"
#include "kernel/driver/apic/local_apic.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

using cinux::base::operator""_KiB;
using cinux::arch::page::Entry;

TEST("percpu: reloading GDT preserves both address bases and the CPU owner") {
    const auto                   kGs         = cinux::arch::per_cpu::ReadGsBase();
    const auto                   kFs         = cinux::arch::ReadMsr(cinux::arch::msr::kFsBase);
    constexpr unsigned long long kThreadBase = 0x12345000;
    cinux::arch::WriteMsr(cinux::arch::msr::kFsBase, kThreadBase);
    cinux::arch::gdt::LoadOwnedGdt();
    cinux::arch::tss::LoadTaskRegister();
    ASSERT_EQ(cinux::arch::per_cpu::ReadGsBase(), kGs);
    ASSERT_EQ(cinux::arch::ReadMsr(cinux::arch::msr::kFsBase), kThreadBase);
    ASSERT_EQ(cinux::arch::per_cpu::PerCpu::self().cpu_id, 0U);
    ASSERT_TRUE(cinux::arch::per_cpu::PerCpu::self().current == nullptr);
    cinux::arch::WriteMsr(cinux::arch::msr::kFsBase, kFs);
}

void check_local_admission(unsigned long long physical) {
    auto& local = cinux::driver::LocalApic::self();
    ASSERT_TRUE(!local.init(0));
    constexpr unsigned int kApicBase  = 0x1B;
    const auto             kPriorBase = cinux::arch::ReadMsr(kApicBase);
    const auto             kEnabled   = cinux::base::bit::MaskBit<unsigned long long>(11);
    cinux::arch::WriteMsr(kApicBase, kPriorBase & ~kEnabled.raw);
    const bool kDisabledRefused = !local.init(physical);
    cinux::arch::WriteMsr(kApicBase, kPriorBase);
    ASSERT_TRUE(kDisabledRefused);
}

TEST("apic: firmware lists the executing BSP and the local MMIO address agrees with its MSR") {
    const auto kTopology =
        cinux::driver::acpi::FindInterruptTopology(cinux::driver::acpi::FindRootPointer());
    ASSERT_TRUE(kTopology.processor_count > 0);
    ASSERT_TRUE(kTopology.io_apic_address != 0);
    auto& local = cinux::driver::LocalApic::self();
    ASSERT_TRUE(local.init(kTopology.local_apic_address));
    bool found = false;
    for (unsigned int index = 0; index < kTopology.processor_count; ++index) {
        found = found || kTopology.processor_apic_ids[index] == local.id();
    }
    ASSERT_TRUE(found);
    ASSERT_TRUE(!local.init(kTopology.local_apic_address + cinux::arch::page::kSize));
    check_local_admission(kTopology.local_apic_address);
    bool timer_override = false;
    for (unsigned int index = 0; index < kTopology.override_count; ++index) {
        const auto& route = kTopology.overrides[index];
        timer_override    = timer_override || (route.source == 0 && route.gsi == 2);
    }
    ASSERT_TRUE(timer_override);
}

TEST("ioapic: GSI base and low-level routes read back independently from MMIO") {
    const auto kOriginal =
        cinux::driver::acpi::FindInterruptTopology(cinux::driver::acpi::FindRootPointer());
    auto topology = kOriginal;
    ASSERT_TRUE(cinux::driver::LocalApic::self().init(topology.local_apic_address));
    auto& controller          = cinux::driver::IoApic::self();
    topology.io_apic_gsi_base = 4;
    topology.override_count   = 16;
    for (unsigned int source = 0; source < 16; ++source) {
        topology.overrides[source] = {
            .source = static_cast<unsigned char>(source),
            .gsi    = source + 4,
            .flags  = 0,
        };
    }
    topology.overrides[3].flags = 15;
    ASSERT_TRUE(controller.init(topology));
    controller.unmask(cinux::interrupt::IrqLine{.value = 3});
    auto* const kWindow =
        cinux::base::PtrAt<volatile unsigned int>(cinux::mm::IoremapVirt(topology.io_apic_address));
    kWindow[0] = 0x16;
    ASSERT_EQ(kWindow[4], 0xA023U);
    kWindow[0] = 0x17;
    cinux::base::bit::BitMask<unsigned int> expected;
    expected.deposit({.low = 24, .width = 8}, cinux::driver::LocalApic::self().id());
    ASSERT_EQ(kWindow[4], expected.raw);
    topology.overrides[3].flags = 2;
    ASSERT_TRUE(!controller.init(topology));
    topology.overrides[3].flags = 8;
    ASSERT_TRUE(!controller.init(topology));
    topology.overrides[3].flags = 0;
    topology.overrides[3].gsi   = 1000;
    ASSERT_TRUE(!controller.init(topology));
    ASSERT_TRUE(controller.init(kOriginal));
}

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
