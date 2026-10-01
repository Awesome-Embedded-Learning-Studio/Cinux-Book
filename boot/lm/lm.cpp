#include "../gdt/gdt.hpp"
#include "early/boot_console.hpp"
#include "layout.hpp"

extern "C" [[gnu::section(".text.lm_entry")]] [[noreturn]] void LmEntry() {
    asm volatile(
        "movw %[data], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        "movl %[stack], %%esp\n"
        :
        : [data] "n"(cinux::boot::gdt::kSelectorData64), [stack] "n"(static_cast<unsigned int>(
                                                             cinux::boot::kPmStackTop))
        : "ax");
    cinux::boot::serial::PutString("[lm] 64-bit world alive\n");
    for (;;) {
        asm volatile("hlt");
    }
}
