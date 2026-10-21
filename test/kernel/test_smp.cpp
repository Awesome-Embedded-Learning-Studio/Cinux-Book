#include <array>
#include <atomic>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"

namespace {

std::atomic<unsigned int> g_cpu_mask{0};
std::atomic<unsigned int> g_ipi_stage{0};
unsigned int              g_increments = 0;
cinux::proc::Spinlock     g_contended_lock;

void check_local_interrupt_fpu() {
    const cinux::arch::IrqGuard             kIrq;
    const std::array<unsigned long long, 2> kExpected{0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL};
    std::array<unsigned long long, 2>       observed{};
    asm volatile("movdqu %1, %%xmm0\n\tint $0xE0\n\tmovdqu %%xmm0, %0"
                 : "=m"(observed)
                 : "m"(kExpected)
                 : "xmm0", "memory");
    ASSERT_TRUE(cinux::base::EqualBytes(kExpected.data(), observed.data(), sizeof(kExpected)));
    asm volatile("movdqu %1, %%xmm0\n\tint $0x30\n\tmovdqu %%xmm0, %0"
                 : "=m"(observed)
                 : "m"(kExpected)
                 : "xmm0", "memory");
    ASSERT_TRUE(cinux::base::EqualBytes(kExpected.data(), observed.data(), sizeof(kExpected)));
}

void check_ipi_idle(unsigned int cpu) {
    const cinux::arch::IrqGuard kIrq;
    if (cpu == 0) {
        while (g_ipi_stage.load(std::memory_order_acquire) != 1) {
            cinux::arch::CpuRelax();
        }
        cinux::arch::smp::NotifyRunnable();
        while (g_ipi_stage.load(std::memory_order_acquire) != 2) {
            cinux::arch::CpuRelax();
        }
        return;
    }
    const auto kTopology =
        cinux::driver::acpi::FindInterruptTopology(cinux::driver::acpi::FindRootPointer());
    auto* const kTimer = cinux::base::PtrAt<volatile unsigned int>(
        cinux::mm::IoremapVirt(kTopology.local_apic_address) + 0x320);
    const auto kPrior   = *kTimer;
    *kTimer             = kPrior | cinux::base::bit::MaskBit<unsigned int>(16).raw;
    const auto& seen    = cinux::arch::per_cpu::PerCpu::self().reschedule_interrupts;
    const auto  kBefore = seen.load(std::memory_order_relaxed);
    g_ipi_stage.store(1, std::memory_order_release);
    while (seen.load(std::memory_order_relaxed) == kBefore) {
        cinux::arch::EnableIrqAndHalt();
        asm volatile("cli" : : : "memory");
    }
    *kTimer = kPrior;
    g_ipi_stage.store(2, std::memory_order_release);
}

bool registered(unsigned long long pid) {
    return cinux::proc::Scheduler::self().with_task(
        pid, []([[maybe_unused]] cinux::proc::Task& task) {});
}

void cross_core_worker() {
    auto& scheduler = cinux::proc::Scheduler::self();
    {
        const cinux::arch::IrqGuard kIrq;
        const unsigned int          kCpu = cinux::arch::per_cpu::PerCpu::self().cpu_id;
        g_cpu_mask.fetch_or(
            cinux::base::bit::MaskBit<unsigned int>(static_cast<unsigned char>(kCpu)).raw,
            std::memory_order_acq_rel);
        while (g_cpu_mask.load(std::memory_order_acquire) != 3) {
            cinux::arch::CpuRelax();
        }
        check_local_interrupt_fpu();
        check_ipi_idle(kCpu);
    }
    for (unsigned int round = 0; round < 256; ++round) {
        {
            const cinux::arch::IrqGuard kIrq;
            ASSERT_EQ(scheduler.current()->on_cpu,
                      static_cast<int>(cinux::arch::per_cpu::PerCpu::self().cpu_id));
        }
        {
            const auto kGuard = g_contended_lock.guard();
            ++g_increments;
        }
        scheduler.yield();
    }
}

void cross_core_dispatch() {
    ASSERT_TRUE(cinux::arch::smp::BootSecondaryCpus());
    auto& scheduler = cinux::proc::Scheduler::self();
    auto* first = cinux::proc::TaskBuilder{}.set_entry(cross_core_worker).set_name("smp-a").build();
    auto* second =
        cinux::proc::TaskBuilder{}.set_entry(cross_core_worker).set_name("smp-b").build();
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(second != nullptr);
    scheduler.seat(*first);
    scheduler.seat(*second);
    const auto kFirstPid  = first->pid;
    const auto kSecondPid = second->pid;
    scheduler.set_preemption(true);
    cinux::arch::smp::EnableSecondaryDispatch();
    while (registered(kFirstPid) || registered(kSecondPid)) {
        scheduler.run_until_done();
    }
    ASSERT_EQ(g_cpu_mask.load(std::memory_order_acquire), 3U);
    {
        const auto kGuard = g_contended_lock.guard();
        ASSERT_EQ(g_increments, 512U);
    }
    ASSERT_TRUE(!registered(kFirstPid));
    ASSERT_TRUE(!registered(kSecondPid));
}

void compare_identity(const cinux::arch::smp::CpuMechanisms& secondary, unsigned int cpu) {
    const auto& bsp = cinux::arch::smp::Mechanisms(0);
    ASSERT_EQ(secondary.magic, 0xA5C0FFEEU);
    ASSERT_EQ(secondary.cpu_id, cpu);
    ASSERT_TRUE(secondary.apic_id != bsp.apic_id);
    ASSERT_EQ(secondary.cr4, bsp.cr4);
}

void compare_owned_tables(const cinux::arch::smp::CpuMechanisms& secondary) {
    const auto& bsp = cinux::arch::smp::Mechanisms(0);
    ASSERT_TRUE(secondary.gdt_base != bsp.gdt_base);
    ASSERT_TRUE(secondary.tss_base != bsp.tss_base);
    ASSERT_EQ(secondary.tss_base, secondary.owned_tss);
    ASSERT_EQ(secondary.task_register, cinux::arch::gdt::kSelectorTss);
    ASSERT_TRUE(secondary.timer_interrupts > 0);
}

void compare_syscall(const cinux::arch::smp::CpuMechanisms& secondary) {
    const auto& bsp = cinux::arch::smp::Mechanisms(0);
    cinux::print::Println("[smp selfcheck] BSP EFER %llx, AP EFER %llx, CR4 %llx/%llx", bsp.efer,
                          secondary.efer, bsp.cr4, secondary.cr4);
    ASSERT_EQ(secondary.efer, bsp.efer);
    ASSERT_EQ(secondary.lstar, bsp.lstar);
    ASSERT_EQ(secondary.star, bsp.star);
    ASSERT_EQ(secondary.sfmask, bsp.sfmask);
}

void ap_mechanism_readback() {
    using namespace cinux::arch::smp;
    ASSERT_TRUE(BootSecondaryCpus());
    ASSERT_EQ(OnlineCpuCount(), 2U);
    const auto& bsp = Mechanisms(0);
    ASSERT_TRUE(bsp.lstar != 0);
    for (unsigned int cpu = 1; cpu < OnlineCpuCount(); ++cpu) {
        const auto& secondary = Mechanisms(cpu);
        compare_identity(secondary, cpu);
        compare_owned_tables(secondary);
        compare_syscall(secondary);
    }
}

void partial_boot_tables_release() {
    auto&         ledger  = cinux::mm::Pmm::self();
    const auto    kBefore = ledger.free_page_count();
    unsigned long held    = 0;
    for (;;) {
        const auto kPage = ledger.allocate_page();
        if (kPage.raw == 0) {
            break;
        }
        *cinux::base::PtrAt<unsigned long>(cinux::mm::DirectMapVirt(kPage.raw)) = held;
        held                                                                    = kPage.raw;
    }
    std::array<bool, 4>          refused{};
    std::array<unsigned long, 4> remaining{};
    for (unsigned int budget = 0; budget < refused.size(); ++budget) {
        if (budget != 0) {
            if (held == 0) {
                break;
            }
            const auto kPage = held;
            held             = *cinux::base::PtrAt<unsigned long>(cinux::mm::DirectMapVirt(held));
            ledger.free_page(cinux::base::PhysAddr{kPage});
        }
        refused[budget]   = !cinux::arch::smp::BootSecondaryCpus();
        remaining[budget] = ledger.free_page_count();
    }
    while (held != 0) {
        const auto kPage = held;
        held             = *cinux::base::PtrAt<unsigned long>(cinux::mm::DirectMapVirt(held));
        ledger.free_page(cinux::base::PhysAddr{kPage});
    }
    ASSERT_EQ(ledger.free_page_count(), kBefore);
    for (unsigned int budget = 0; budget < refused.size(); ++budget) {
        ASSERT_TRUE(refused[budget]);
        ASSERT_EQ(remaining[budget], static_cast<unsigned long>(budget));
    }
}

[[gnu::used, gnu::section(".test_cases")]] constinit const cinux::test::TestCase kApSelfcheck{
    .name = "smp: online APs read back CR4 EFER LSTAR STAR SFMASK against the BSP",
    .body = ap_mechanism_readback,
};

[[gnu::used, gnu::section(".test_cases")]] constinit const cinux::test::TestCase kCrossCore{
    .name = "smp: two CPUs dispatch shared tasks and contend on a real spinlock",
    .body = cross_core_dispatch,
};

[[gnu::used, gnu::section(".test_cases")]] constinit const cinux::test::TestCase kPartialBoot{
    .name = "smp: every partial trampoline mapping allocation is returned on failure",
    .body = partial_boot_tables_release,
};

}
