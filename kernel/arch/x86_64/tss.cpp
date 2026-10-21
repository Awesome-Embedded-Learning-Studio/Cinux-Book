#include "kernel/arch/x86_64/tss.hpp"

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/per_cpu_config.hpp"

namespace cinux::arch::tss {

Tss& Tss::self() {
    static constinit Tss blocks[per_cpu::kMaxCpus]{};
    return blocks[per_cpu::PerCpu::self().cpu_id];
}

void LoadTaskRegister() {
    Tss::self().io_bitmap_end = sizeof(Tss);
    asm volatile("ltr %0" : : "r"(gdt::kSelectorTss) : "memory");
}

unsigned short ReadTaskRegister() {
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned short selector = 0;
    asm volatile("str %0" : "=r"(selector));
    return selector;
}

}  // namespace cinux::arch::tss
