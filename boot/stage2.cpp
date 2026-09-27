#include "early/boot_console.hpp"

asm(".section .text.boot, \"ax\"\n"
    ".global stage2_start\n"
    "stage2_start:\n"
    "   call Stage2Main\n"
    "0: hlt\n"
    "   jmp 0b\n"
    ".text\n");

extern "C" [[noreturn]] void Stage2Main() {
    cinux::boot::serial::PutString("[stage2] echo alive\n");
    for (;;) {
        asm volatile("hlt");
    }
}
