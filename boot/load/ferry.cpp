#include "load/loader.hpp"

extern "C" {
unsigned g_dst;
unsigned g_src;
unsigned g_len;
}

extern "C" void FerryEntry32();
extern "C" void PmVisit();

namespace cinux::boot::load {
void RunFerry(unsigned dst, unsigned src, unsigned len) {
    g_dst = dst;
    g_src = src;
    g_len = len;
    PmVisit();
}
}  // namespace cinux::boot::load

asm(".section .text.ferry,\"ax\"\n"
    ".global PmVisit\n"
    ".global FerryExit16\n"
    "PmVisit:\n"
    "   cli\n"
    "   movl %cr0, %eax\n"
    "   orb $1, %al\n"
    "   movl %eax, %cr0\n"
    "   ljmp $0x08, $FerryEntry32\n"
    "FerryExit16:\n"
    "   movl %cr0, %eax\n"
    "   andb $0xfe, %al\n"
    "   movl %eax, %cr0\n"
    "   ljmp $0x0000, $1f\n"
    "1: xorw %ax, %ax\n"
    "   movw %ax, %ds\n"
    "   movw %ax, %es\n"
    "   movw %ax, %ss\n"
    "   .byte 0x66, 0xc3\n"
    ".text\n");
