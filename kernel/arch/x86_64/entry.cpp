#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/boot/boot_info.hpp"

extern "C" unsigned char g_kernel_bss_start[];
extern "C" unsigned char g_kernel_bss_end[];
extern "C" unsigned char g_kernel_stack_top[];

namespace kernel {
void Main(const cinux::boot::BootInfo& info);
}

namespace {

void save_handoff_and_start(unsigned long long info_addr) {
    // NOLINTNEXTLINE(clang-analyzer-security.PointerSub)
    auto const kBssBytes = static_cast<unsigned long>(g_kernel_bss_end - g_kernel_bss_start);
    cinux::base::SetBytes(g_kernel_bss_start, 0, kBssBytes);
    cinux::arch::gdt::LoadOwnedGdt();
    cinux::arch::isr::InstallExceptionStubs();
    cinux::arch::idt::LoadIdt();
    kernel::Main(*cinux::base::PtrAt<cinux::boot::BootInfo>(info_addr));
    cinux::arch::Halt();
}

}  // namespace

extern "C" void KernelEntry() {
    asm volatile(
        "movq %%cr0, %%rax\n\t"
        "andq $~0x4, %%rax\n\t"
        "orq $0x2, %%rax\n\t"
        "movq %%rax, %%cr0\n\t"
        "movq %%cr4, %%rax\n\t"
        "orq $0x600, %%rax\n\t"
        "movq %%rax, %%cr4\n\t"
        "leaq g_kernel_stack_top(%%rip), %%rsp\n\t"
        "xorq %%rbp, %%rbp"
        :
        :
        : "rax", "memory");
    // NOLINTBEGIN(misc-const-correctness)
    unsigned long long info = 0;
    // NOLINTEND(misc-const-correctness)
    asm volatile("movq %%rdi, %0" : "=r"(info) : : "memory");
    save_handoff_and_start(info);
    __builtin_unreachable();
}
