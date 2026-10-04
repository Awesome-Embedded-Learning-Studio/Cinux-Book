#include "kernel/arch/x86_64/gdt.hpp"

namespace cinux::arch::gdt {

namespace {

KernelGdt g_own_gdt = MakeKernelGdt();

}

void LoadOwnedGdt() {
    TablePointer const       kPointer = {.limit = sizeof(KernelGdt) - 1,
                                         .base  = reinterpret_cast<unsigned long long>(&g_own_gdt)};
    unsigned long long const kCode    = kSelectorCode;
    unsigned long long const kData    = kSelectorData;
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
        "movw %%ax, %%ss\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs"
        :
        : [pointer] "m"(kPointer), [code] "r"(kCode), [data] "r"(kData)
        : "rax", "memory");
}

}  // namespace cinux::arch::gdt
