#include "../gdt/gdt.hpp"
#include "layout.hpp"
#include "lm.hpp"

extern "C" [[noreturn]] void EnterLongMode() {
    asm volatile(
        "movl %[pml4], %%eax\n"
        "movl %%eax, %%cr3\n"
        "movl %%cr4, %%eax\n"
        "orl %[pae], %%eax\n"
        "movl %%eax, %%cr4\n"
        "movl %[efer], %%ecx\n"
        "rdmsr\n"
        "orl %[lme], %%eax\n"
        "wrmsr\n"
        "movl %%cr0, %%eax\n"
        "orl %[pg], %%eax\n"
        "movl %%eax, %%cr0\n"
        "ljmp %[code64], %[entry]\n"
        :
        : [pml4] "n"(cinux::boot::lm::kPml4Phys), [pae] "n"(cinux::boot::lm::kCr4Pae),
          [efer] "n"(cinux::boot::lm::kMsrEfer), [lme] "n"(cinux::boot::lm::kEferLme),
          [pg] "n"(cinux::boot::lm::kCr0Pg), [code64] "n"(cinux::boot::gdt::kSelectorCode64),
          [entry] "n"(cinux::boot::kLmEntryVma)
        : "ax", "cx", "dx", "memory");
    __builtin_unreachable();
}
