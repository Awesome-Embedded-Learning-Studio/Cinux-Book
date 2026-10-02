#include "../early/console.hpp"
#include "../gdt/gdt.hpp"
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
    cinux::console::PutString("[lm] 64-bit world alive\n");
    unsigned long const kEntryTarget =
        cinux::boot::kHighHalfBase + cinux::boot::LoadWord(cinux::boot::kHandoffMailboxEntry);
    unsigned long const kInfoAddress =
        cinux::boot::kHighHalfBase + cinux::boot::LoadWord(cinux::boot::kHandoffMailboxInfo);
    asm volatile(
        "movq %[entry], %%rax\n"
        "movq %[info], %%rdi\n"
        "jmpq *%%rax\n"
        :
        : [entry] "r"(kEntryTarget), [info] "r"(kInfoAddress)
        : "rax", "rdi", "memory");
    __builtin_unreachable();
}
