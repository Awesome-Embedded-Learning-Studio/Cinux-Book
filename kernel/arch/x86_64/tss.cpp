#include "kernel/arch/x86_64/tss.hpp"

#include "kernel/arch/x86_64/gdt.hpp"

namespace cinux::arch::tss {

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
