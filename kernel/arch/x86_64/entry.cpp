#include "cinux/ptr.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/console.hpp"

extern "C" unsigned char g_kernel_bss_start[];
extern "C" unsigned char g_kernel_bss_end[];

extern "C" {
void ExcDe();
void ExcUd();
void ExcDf();
void ExcGp();
void ExcPf();
}

namespace kernel {
void Main(const cinux::boot::BootInfo& info);
}

extern "C" void ReportException(unsigned long long vector, unsigned long long rip);

namespace {

asm(".section .text.exc,\"ax\"\n"
    ".global ExcDe\n"
    "ExcDe:\n  pushq $0\n  pushq $0\n  jmp ExceptionCommon\n"
    ".global ExcUd\n"
    "ExcUd:\n  pushq $0\n  pushq $6\n  jmp ExceptionCommon\n"
    ".global ExcDf\n"
    "ExcDf:\n  pushq $0\n  pushq $8\n  jmp ExceptionCommon\n"
    ".global ExcGp\n"
    "ExcGp:\n  pushq $13\n  jmp ExceptionCommon\n"
    ".global ExcPf\n"
    "ExcPf:\n  pushq $14\n  jmp ExceptionCommon\n"
    ".text\n");

struct [[gnu::packed]] IdtEntry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char  ist;
    unsigned char  type_attr;
    unsigned short offset_mid;
    unsigned int   offset_high;
    unsigned int   reserved;
};

IdtEntry g_idt[32];

struct [[gnu::packed]] IdtPointer {
    unsigned short     limit;
    unsigned long long base;
};

void memory_zero(void* target, unsigned long bytes) {
    auto* dest = reinterpret_cast<unsigned char*>(target);
    auto* scan = dest;
    while (bytes-- > 0) {
        *scan++ = 0;
    }
}

void install_handler(unsigned char vector, void* handler) {
    auto const kAddr          = reinterpret_cast<unsigned long>(handler);
    g_idt[vector].offset_low  = static_cast<unsigned short>(kAddr & 0xFFFF);
    g_idt[vector].selector    = 0x18;
    g_idt[vector].ist         = 0;
    g_idt[vector].type_attr   = 0x8E;
    g_idt[vector].offset_mid  = static_cast<unsigned short>((kAddr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = static_cast<unsigned int>(kAddr >> 32);
    g_idt[vector].reserved    = 0;
}

void save_handoff_and_start(unsigned long long info_addr) {
    // NOLINTNEXTLINE(clang-analyzer-security.PointerSub)
    auto const kBssBytes = static_cast<unsigned long>(g_kernel_bss_end - g_kernel_bss_start);
    memory_zero(g_kernel_bss_start, kBssBytes);
    install_handler(0, reinterpret_cast<void*>(&ExcDe));
    install_handler(6, reinterpret_cast<void*>(&ExcUd));
    install_handler(8, reinterpret_cast<void*>(&ExcDf));
    install_handler(13, reinterpret_cast<void*>(&ExcGp));
    install_handler(14, reinterpret_cast<void*>(&ExcPf));
    IdtPointer const kIdtr = {.limit = sizeof(g_idt) - 1,
                              .base  = reinterpret_cast<unsigned long long>(g_idt)};
    asm volatile("lidt %0" : : "m"(kIdtr) : "memory");
    kernel::Main(*cinux::base::PtrAt<cinux::boot::BootInfo>(info_addr));
    cinux::console::Halt();
}

}  // namespace

extern "C" [[gnu::section(".text.exc")]] void ExceptionCommon() {
    // NOLINTBEGIN(misc-const-correctness)
    unsigned long long vector = 0;
    unsigned long long rip    = 0;
    // NOLINTEND(misc-const-correctness)
    asm volatile(
        "movq 8(%%rsp), %0\n"
        "movq 16(%%rsp), %1\n"
        : "=r"(vector), "=r"(rip)
        :
        : "memory");
    ReportException(vector, rip);
}

extern "C" void KernelEntry() {
    asm volatile(
        "movabsq $0x90000, %%rsp\n"
        "xorq %%rbp, %%rbp\n"
        :
        :
        : "memory");
    // NOLINTBEGIN(misc-const-correctness)
    unsigned long long info = 0;
    // NOLINTEND(misc-const-correctness)
    asm volatile("movq %%rdi, %0\n" : "=r"(info) : : "memory");
    save_handoff_and_start(info);
    __builtin_unreachable();
}
