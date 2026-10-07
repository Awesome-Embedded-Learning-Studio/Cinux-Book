#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/boot/boot_info.hpp"

extern "C" unsigned char g_kernel_bss_start[];
extern "C" unsigned char g_kernel_bss_end[];

namespace kernel {
void Main(const cinux::boot::BootInfo& info);
}

// KernelEntry in entry.S establishes the stack and preserves the handoff in rdi.
extern "C" [[noreturn]] void KernelStart(unsigned long long info_addr) {
    // NOLINTNEXTLINE(clang-analyzer-security.PointerSub)
    auto const kBssBytes = static_cast<unsigned long>(g_kernel_bss_end - g_kernel_bss_start);
    cinux::base::SetBytes(g_kernel_bss_start, 0, kBssBytes);
    cinux::arch::gdt::LoadOwnedGdt();
    cinux::arch::tss::LoadTaskRegister();
    cinux::arch::per_cpu::InstallKernelGs();
    cinux::arch::isr::InstallExceptionStubs();
    cinux::arch::idt::LoadIdt();
    kernel::Main(*cinux::base::PtrAt<cinux::boot::BootInfo>(info_addr));
    cinux::arch::Halt();
}
