#include "kernel/arch/x86_64/smp.hpp"

#include <atomic>
#include <cstdint>

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "cinux/scoped_guard.hpp"
#include "cinux/time.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/irq_stubs.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/per_cpu_config.hpp"
#include "kernel/arch/x86_64/smp_config.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/arch/x86_64/vector_config.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/driver/apic/local_apic.hpp"
#include "kernel/driver/pit/pit.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"

extern "C" {
extern const unsigned char kApTrampolineStart[] asm("ApTrampolineStart");
extern const unsigned char kApTrampolineEnd[] asm("ApTrampolineEnd");
extern const unsigned char kApBootRoot[] asm("ApBootRoot");
extern const unsigned char kApKernelRoot[] asm("ApKernelRoot");
extern const unsigned char kApStack[] asm("ApStack");
extern const unsigned char kApCpu[] asm("ApCpu");
extern const unsigned char kApEntry[] asm("ApEntry");
extern const unsigned char kApEfer[] asm("ApEfer");
extern const unsigned char kApCr4[] asm("ApCr4");
void                       ApEntryLong();
}

namespace cinux::arch::smp {

namespace {

constexpr unsigned int kReadyMagic     = 0xA5C0FFEE;
constexpr uint32_t     kInitialCounter = base::bit::Ones<uint32_t>(32);
using namespace base::time;
constexpr auto                kCalibration = 10000_us;
constexpr auto                kAssertLevel = base::bit::MaskBit<uint32_t>(14);
constexpr base::bit::BitRange kDeliveryMode{.low = 8, .width = 3};
constexpr unsigned int        kInitMode    = 5;
constexpr unsigned int        kStartupMode = 6;
alignas(16) unsigned char g_ap_stacks[per_cpu::kMaxCpus][proc::kStackBytes]{};
CpuMechanisms             g_mechanisms[per_cpu::kMaxCpus]{};
std::atomic<bool>         g_online[per_cpu::kMaxCpus]{};
std::atomic<unsigned int> g_online_count{1};
uint32_t                  g_timer_count = 0;
bool                      g_started     = false;
std::atomic<bool>         g_dispatch{false};
unsigned char             g_trampoline_image[page::kSize]{};

struct TemporaryRootPolicy {
    struct State {
        unsigned long& root;
    };
    static void enter(State& state) {
        mm::KernelTables tables;
        state.root = tables.take_page();
    }
    static void exit(State& state) {
        mm::KernelTables tables;
        auto             page = state.root;
        for (unsigned int level = 0; page != 0 && level < 4; ++level) {
            const auto kNext = mm::walk::EntryTarget(tables.table(page)[0]);
            mm::Pmm::self().free_page(base::PhysAddr{page});
            page = kNext;
        }
    }
};

unsigned long symbol_offset(const unsigned char* symbol) {
    return reinterpret_cast<unsigned long>(symbol) -
           reinterpret_cast<unsigned long>(kApTrampolineStart);
}

CpuMechanisms capture() {
    unsigned long long cr4;
    asm volatile("movq %%cr4, %0" : "=r"(cr4));
    gdt::TablePointer pointer{};
    gdt::StoreGdtSnapshot(&pointer);
    const auto         kTr = tss::ReadTaskRegister();
    gdt::TssDescriptor descriptor{};
    base::CopyBytes(&descriptor, base::PtrAt<const void>(pointer.base + kTr), sizeof(descriptor));
    base::bit::BitMask<uint64_t> tss_base;
    tss_base.deposit({.low = 0, .width = 16}, descriptor.low.base_low);
    tss_base.deposit({.low = 16, .width = 8}, descriptor.low.base_mid);
    tss_base.deposit({.low = 24, .width = 8}, descriptor.low.base_high);
    tss_base.deposit({.low = 32, .width = 32}, descriptor.base_high);
    return {
        .cr4           = cr4,
        .efer          = ReadMsr(msr::kEfer),
        .lstar         = ReadMsr(msr::kLstar),
        .star          = ReadMsr(msr::kStar),
        .sfmask        = ReadMsr(msr::kSfmask),
        .gdt_base      = pointer.base,
        .tss_base      = tss_base.raw,
        .owned_tss     = reinterpret_cast<uint64_t>(&tss::Tss::self()),
        .task_register = kTr,
        .timer_interrupts =
            per_cpu::PerCpu::self().local_timer_interrupts.load(std::memory_order_relaxed),
        .cpu_id  = per_cpu::PerCpu::self().cpu_id,
        .apic_id = driver::LocalApic::self().id(),
        .magic   = kReadyMagic,
    };
}

void patch_word(unsigned char* image, const unsigned char* field, uint64_t value) {
    const auto kOffset = static_cast<uint32_t>(symbol_offset(field));
    base::WriteWord32(image, kOffset, static_cast<uint32_t>(value));
    base::WriteWord32(image, kOffset + sizeof(uint32_t), static_cast<uint32_t>(value >> 32U));
}

uint32_t command(unsigned int mode) {
    base::bit::BitMask<uint32_t> word{kAssertLevel};
    word.deposit(kDeliveryMode, mode);
    return word.raw;
}

}

extern "C" [[noreturn]] void ApMain(unsigned int cpu) {
    per_cpu::InstallKernelGs(cpu);
    gdt::LoadOwnedGdt();
    tss::LoadTaskRegister();
    idt::LoadIdt();
    asm volatile("fninit" : : : "memory");
    usermode::EnableFastSystemCalls();
    auto& local                     = driver::LocalApic::self();
    per_cpu::PerCpu::self().apic_id = local.id();
    local.enable(irq::kSpuriousVector);
    local.start_timer(kTimerVector, g_timer_count, true);
    while (per_cpu::PerCpu::self().local_timer_interrupts.load(std::memory_order_relaxed) == 0) {
        EnableIrqAndHalt();
        asm volatile("cli" : : : "memory");
    }
    g_mechanisms[cpu] = capture();
    g_online[cpu].store(true, std::memory_order_release);
    g_online_count.fetch_add(1, std::memory_order_release);
    for (;;) {
        if (g_dispatch.load(std::memory_order_acquire)) {
            proc::Scheduler::self().run_until_done();
        }
        EnableIrqAndHalt();
        asm volatile("cli" : : : "memory");
    }
}

bool BootSecondaryCpus() {
    if (g_started) {
        return true;
    }
    const auto kTopology = driver::acpi::FindInterruptTopology(driver::acpi::FindRootPointer());
    if (kTopology.processor_count == 0) {
        return false;
    }
    irq::InstallLocalInterrupts();
    usermode::EnableFastSystemCalls();
    auto& local                     = driver::LocalApic::self();
    per_cpu::PerCpu::self().apic_id = local.id();
    g_mechanisms[0]                 = capture();
    local.start_timer(kTimerVector, kInitialCounter, false);
    driver::Pit::self().wait_microseconds(kCalibration);
    g_timer_count = kInitialCounter - local.timer_remaining();
    local.stop_timer();
    if (g_timer_count == 0) {
        return false;
    }
    base::PhysAddr                               root;
    const base::ScopedGuard<TemporaryRootPolicy> kTemporary(root.raw);
    if (root.raw == 0 || root.raw > base::bit::Ones<uint32_t>(32)) {
        return false;
    }
    mm::KernelTables tables;
    base::CopyBytes(tables.table(root.raw), tables.table(mm::KernelPageRoot()), page::kSize);
    if (!mm::walk::MapPage(tables, root.raw, kTrampolinePhysical, kTrampolinePhysical)) {
        return false;
    }
    auto* const kImage      = g_trampoline_image;
    const auto  kImageBytes = symbol_offset(kApTrampolineEnd);
    base::safety::Check(kImageBytes <= page::kSize, "AP trampoline exceeds one SIPI page");
    base::CopyBytes(kImage, kApTrampolineStart, kImageBytes);
    base::WriteWord32(kImage, static_cast<uint32_t>(symbol_offset(kApBootRoot)),
                      static_cast<uint32_t>(root.raw));
    base::WriteWord32(kImage, static_cast<uint32_t>(symbol_offset(kApEfer)),
                      static_cast<uint32_t>(g_mechanisms[0].efer & ~msr::kEferLongModeActive));
    base::WriteWord32(kImage, static_cast<uint32_t>(symbol_offset(kApCr4)),
                      static_cast<uint32_t>(g_mechanisms[0].cr4));
    patch_word(kImage, kApKernelRoot, mm::KernelPageRoot());
    patch_word(kImage, kApEntry, reinterpret_cast<uint64_t>(&ApEntryLong));
    unsigned int cpu = 1;
    for (unsigned int index = 0; index < kTopology.processor_count; ++index) {
        const uint8_t kApic = kTopology.processor_apic_ids[index];
        if (kApic == local.id()) {
            continue;
        }
        if (cpu == per_cpu::kMaxCpus) {
            break;
        }
        base::WriteWord32(kImage, static_cast<uint32_t>(symbol_offset(kApCpu)), cpu);
        patch_word(kImage, kApStack,
                   reinterpret_cast<uint64_t>(g_ap_stacks[cpu]) + proc::kStackBytes);
        base::CopyBytes(base::PtrAt<unsigned char>(mm::DirectMapVirt(kTrampolinePhysical)), kImage,
                        kImageBytes);
        local.send(kApic, command(kInitMode));
        driver::Pit::self().wait_microseconds(10000_us);
        const uint32_t kStartup = command(kStartupMode) | (kTrampolinePhysical / page::kSize);
        local.send(kApic, kStartup);
        driver::Pit::self().wait_microseconds(200_us);
        local.send(kApic, kStartup);
        while (g_online_count.load(std::memory_order_acquire) <= cpu) {
            CpuRelax();
        }
        ++cpu;
    }
    g_started = true;
    return true;
}

unsigned int OnlineCpuCount() {
    return g_online_count.load(std::memory_order_acquire);
}

void NotifyRunnable() {
    if (OnlineCpuCount() < 2) {
        return;
    }
    const IrqGuard kIrq;
    constexpr auto kOthers = base::bit::MaskBit<uint32_t>(18) | base::bit::MaskBit<uint32_t>(19);
    driver::LocalApic::self().send(0, kRescheduleVector | kOthers.raw);
}

void EnableSecondaryDispatch() {
    g_dispatch.store(true, std::memory_order_release);
    NotifyRunnable();
}

const CpuMechanisms& Mechanisms(unsigned int cpu) {
    base::safety::Check(cpu < OnlineCpuCount(), "mechanism read of offline CPU");
    if (cpu != 0) {
        base::safety::Check(g_online[cpu].load(std::memory_order_acquire),
                            "AP readback not published");
    }
    return g_mechanisms[cpu];
}

}
