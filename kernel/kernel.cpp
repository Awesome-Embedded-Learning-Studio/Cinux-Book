#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/rtc/rtc.hpp"
#include "kernel/init/init_sequence.hpp"
#include "kernel/init/init_thread.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/time/tick_config.hpp"

namespace kernel {

using namespace cinux::console;
using cinux::print::Println;

// NOLINTNEXTLINE(misc-use-internal-linkage)
void Main(const cinux::boot::BootInfo& boot_info) {
    auto const* const kInfo  = cinux::init::RunInitSequence(&boot_info);
    cinux::mm::Pmm&   ledger = cinux::mm::Pmm::self();
    Println("[kern] 64-bit C++ world alive");
    Println("[kern] gdt+idt self-owned, sse on");
    Println("[kern] tss loaded tr=%X gs base %X efer %X", cinux::arch::tss::ReadTaskRegister(),
            cinux::arch::per_cpu::ReadGsBase(), cinux::arch::ReadMsr(cinux::arch::msr::kEfer));
    Println("[kern] kernel gs base %X", cinux::arch::per_cpu::ReadKernelGsBase());
    Println("[kern] bootinfo: %u e820 entries, fb %u*%u*%u", kInfo->e820_count,
            kInfo->framebuffer.width, kInfo->framebuffer.height, kInfo->framebuffer.bpp);
    if (cinux::console::TextConsole::self().alive()) {
        Println("[kern] screen console %u cols %u rows",
                cinux::console::TextConsole::self().columns(),
                cinux::console::TextConsole::self().rows());
    }
    Println("[kern] kernel at %X size %X entry %X", kInfo->kernel_paddr, kInfo->kernel_mem_size,
            kInfo->kernel_entry);
    cinux::base::PhysAddr const kProbe = ledger.allocate_page();
    ledger.free_page(kProbe);
    Println("[kern] pmm: %u pages free (probe %X ok)", ledger.free_page_count(), kProbe.raw);
    auto const kMagic = *cinux::base::PtrAt<const uint32_t>(
        cinux::mm::DirectMapVirt(static_cast<unsigned long>(kInfo->kernel_paddr)));
    Println("[kern] vmm: address space up, magic %X", kMagic);
    auto* const kHeapProbe = new unsigned long;
    *kHeapProbe            = 0x114514UL;
    Println("[kern] heap: new/delete live (probe %X)", *kHeapProbe);
    delete kHeapProbe;
    Println("[kern] irq on, tick %uHz", cinux::time::kTickHz.value);
    Println("[kern] rtc wall clock %llu", cinux::driver::Rtc::self().now_seconds());
    Println("[kern] keyboard on, type to echo");
    Println("[kern] boot task (pid 0) parks here, kernel_init (pid 1) takes over");
    cinux::proc::InstallKernelSwitchSink();
    cinux::proc::Scheduler::self().set_preemption(true);
    cinux::init::StartInitThread();
    for (;;) {
        cinux::proc::Scheduler::self().run_until_done();
        cinux::arch::EnableIrqAndHalt();
    }
}

}  // namespace kernel
