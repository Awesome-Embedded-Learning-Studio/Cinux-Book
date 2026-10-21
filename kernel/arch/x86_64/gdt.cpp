#include "kernel/arch/x86_64/gdt.hpp"

#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/per_cpu_config.hpp"
#include "kernel/arch/x86_64/tss.hpp"

namespace cinux::arch::gdt {

namespace {

constinit KernelGdt g_gdt_blocks[per_cpu::kMaxCpus] = {};

}  // namespace

void LoadOwnedGdt() {
    auto& owned_gdt     = g_gdt_blocks[per_cpu::PerCpu::self().cpu_id];
    owned_gdt           = MakeKernelGdt();
    auto const kTss     = MakeTssDescriptor(reinterpret_cast<unsigned long long>(&tss::Tss::self()),
                                            sizeof(tss::Tss) - 1);
    owned_gdt.tss_low   = kTss.low;
    owned_gdt.tss_upper = kTss.base_high;
    owned_gdt.tss_zero  = kTss.reserved;
    TablePointer const kPointer = {
        .limit = sizeof(KernelGdt) - 1,
        .base  = reinterpret_cast<unsigned long long>(&owned_gdt),
    };
    unsigned long long const kCode = kSelectorCode;
    unsigned long long const kData = kSelectorData;
    asm volatile(
        "lgdtq %[pointer]\n\t"
        "pushq %[code]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %w[data], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss"
        :
        : [pointer] "m"(kPointer), [code] "r"(kCode), [data] "r"(kData)
        : "rax", "memory");
}

}  // namespace cinux::arch::gdt
