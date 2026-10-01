#include "cinux/ptr.hpp"

extern "C" {
extern unsigned g_dst;
extern unsigned g_src;
extern unsigned g_len;
void            CopyFlat(unsigned dst, unsigned src, unsigned len);
}

void CopyFlat(unsigned dst, unsigned src, unsigned len) {
    auto*       destination = cinux::base::PtrAt<unsigned>(dst);
    auto const* source      = cinux::base::PtrAt<unsigned>(src);
    for (unsigned i = 0; i < len / 4; ++i) {
        destination[i] = source[i];
    }
}

extern "C" [[gnu::naked]] void FerryEntry32() {
    asm volatile(
        "movw $0x10, %ax\n"
        "movw %ax, %ds\n"
        "movw %ax, %es\n"
        "movw %ax, %ss\n"
        "subl $12, %esp\n"
        "movl g_dst, %eax\n"
        "movl %eax, (%esp)\n"
        "movl g_src, %eax\n"
        "movl %eax, 4(%esp)\n"
        "movl g_len, %eax\n"
        "movl %eax, 8(%esp)\n"
        "call CopyFlat\n"
        "addl $12, %esp\n"
        "ljmp $0x28, $FerryExit16\n");
}
