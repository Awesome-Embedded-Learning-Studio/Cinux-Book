#include "cinux/addr.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/irq_stubs.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/console.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/pit.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/time/tick.hpp"
#include "kernel/time/tick_config.hpp"

namespace kernel {

using namespace cinux::console;
using cinux::print::Println;

// NOLINTNEXTLINE(misc-use-internal-linkage)
void Main(const cinux::boot::BootInfo& info) {
    InitConsole();
    Println("[kern] 64-bit C++ world alive");
    Println("[kern] gdt+idt self-owned, sse on");
    Println("[kern] bootinfo: %u e820 entries, fb %u*%u*%u", info.e820_count,
            info.framebuffer.width, info.framebuffer.height, info.framebuffer.bpp);
    Println("[kern] kernel at %X size %X entry %X", info.kernel_paddr, info.kernel_mem_size,
            info.kernel_entry);
    cinux::mm::Pmm& ledger = cinux::mm::Pmm::self();
    if (!ledger.init(info)) {
        Println("[kern] pmm init failed");
        cinux::arch::Halt();
    }
    cinux::base::PhysAddr const kProbe = ledger.allocate_page();
    ledger.free_page(kProbe);
    Println("[kern] pmm: %u pages free (probe %X ok)", ledger.free_page_count(), kProbe.raw);

    cinux::arch::Pic::self().remap();
    cinux::time::Tick::self().init(cinux::driver::Pit::self());
    cinux::arch::irq::InstallIrqStubs();
    cinux::interrupt::Irq::self().enable_line(cinux::interrupt::IrqLine{.value = 0});
    asm volatile("sti" : : : "memory");
    Println("[kern] irq on, tick %uHz", cinux::time::kTickHz.value);
    cinux::arch::Halt();
}

}  // namespace kernel
