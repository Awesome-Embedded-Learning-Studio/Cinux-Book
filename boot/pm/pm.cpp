#include "early/boot_console.hpp"
#include "layout.hpp"

extern "C" void              BuildTemporaryPageTables();
extern "C" [[noreturn]] void EnterLongMode();

extern "C" [[noreturn]] void PmEntry() {
    asm volatile(
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        "movl %[stack], %%esp\n"
        :
        : [stack] "n"(cinux::boot::kPmStackTop)
        : "ax");
    cinux::boot::serial::PutString("[pm] 32-bit world alive\n");
    BuildTemporaryPageTables();
    EnterLongMode();
}
