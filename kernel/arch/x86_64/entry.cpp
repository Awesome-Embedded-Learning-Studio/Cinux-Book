#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/boot/boot_info.hpp"

extern "C" unsigned char g_kernel_bss_start[];
extern "C" unsigned char g_kernel_bss_end[];

/// The stack-canary guard word, defined beside the panic tripwire;
/// seeded here from the cycle counter, after the clear and before any
/// protected function can run.
// NOLINTNEXTLINE(bugprone-reserved-identifier) the ABI name
extern "C" {
unsigned long long __stack_chk_guard = 0xDEAD'BEEF'CAFE'F00DULL;
}

/// One free-running count; different on every boot by construction.
unsigned long long cycle_count() {
    unsigned int low  = 0;
    unsigned int high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<unsigned long long>(high) << 32U) | static_cast<unsigned long long>(low);
}

namespace kernel {
void Main(const cinux::boot::BootInfo& info);
}

// KernelEntry in entry.S establishes the stack and preserves the handoff in rdi.
extern "C" [[noreturn]] void KernelStart(unsigned long long info_addr) {
    // NOLINTNEXTLINE(clang-analyzer-security.PointerSub)
    auto const kBssBytes = static_cast<unsigned long>(g_kernel_bss_end - g_kernel_bss_start);
    cinux::base::SetBytes(g_kernel_bss_start, 0, kBssBytes);
    __stack_chk_guard = cycle_count() ^ 0xDEAD'BEEF'CAFE'F00DULL;
    cinux::arch::WriteCr0(cinux::arch::ReadCr0() | cinux::arch::kCr0WriteProtect);
    cinux::arch::per_cpu::InstallKernelGs();
    cinux::arch::gdt::LoadOwnedGdt();
    cinux::arch::tss::LoadTaskRegister();
    cinux::arch::isr::InstallExceptionStubs();
    cinux::arch::idt::LoadIdt();
    kernel::Main(*cinux::base::PtrAt<cinux::boot::BootInfo>(info_addr));
    cinux::arch::Halt();
}
