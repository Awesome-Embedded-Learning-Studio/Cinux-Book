#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/irq_stubs.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/console/console.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/keyboard.hpp"
#include "kernel/driver/pit.hpp"
#include "kernel/driver/serial.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/mm/heap_runtime.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/time/tick.hpp"
#include "kernel/time/tick_config.hpp"

namespace kernel {

using namespace cinux::console;
using cinux::print::Println;

// NOLINTNEXTLINE(misc-use-internal-linkage)
void Main(const cinux::boot::BootInfo& boot_info) {
    cinux::driver::SerialInit();
    cinux::mm::Pmm& ledger = cinux::mm::Pmm::self();
    if (!ledger.init(boot_info)) {
        Println("[kern] pmm init failed");
        cinux::arch::Halt();
    }
    const cinux::boot::BootInfo* info = cinux::mm::BringUpAddressSpace(boot_info);
    if (info == nullptr) {
        Println("[kern] address space bring-up failed");
        cinux::arch::Halt();
    }
    cinux::console::TextConsole::self().init(info->framebuffer);
    Println("[kern] 64-bit C++ world alive");
    Println("[kern] gdt+idt self-owned, sse on");
    Println("[kern] bootinfo: %u e820 entries, fb %u*%u*%u", info->e820_count,
            info->framebuffer.width, info->framebuffer.height, info->framebuffer.bpp);
    if (cinux::console::TextConsole::self().alive()) {
        Println("[kern] screen console %u cols %u rows",
                cinux::console::TextConsole::self().columns(),
                cinux::console::TextConsole::self().rows());
    }
    Println("[kern] kernel at %X size %X entry %X", info->kernel_paddr, info->kernel_mem_size,
            info->kernel_entry);
    cinux::base::PhysAddr const kProbe = ledger.allocate_page();
    ledger.free_page(kProbe);
    Println("[kern] pmm: %u pages free (probe %X ok)", ledger.free_page_count(), kProbe.raw);
    auto const kMagic = *cinux::base::PtrAt<const uint32_t>(
        cinux::mm::DirectMapVirt(static_cast<unsigned long>(info->kernel_paddr)));
    Println("[kern] vmm: address space up, magic %X", kMagic);
    if (!cinux::mm::BringUpHeap()) {
        Println("[kern] heap bring-up failed");
        cinux::arch::Halt();
    }
    auto* const kHeapProbe = new unsigned long;
    *kHeapProbe            = 0x114514UL;
    Println("[kern] heap: new/delete live (probe %X)", *kHeapProbe);
    delete kHeapProbe;

    cinux::arch::Pic::self().remap();
    cinux::time::Tick::self().init(cinux::driver::Pit::self());
    cinux::arch::irq::InstallIrqStubs();
    cinux::interrupt::Irq::self().enable_line(cinux::interrupt::IrqLine{.value = 0});
    cinux::driver::Keyboard& keyboard = cinux::driver::Keyboard::self();
    if (!keyboard.init()) {
        Println("[kern] keyboard self-test failed");
        cinux::arch::Halt();
    }
    keyboard.attach();
    asm volatile("sti" : : : "memory");
    Println("[kern] irq on, tick %uHz", cinux::time::kTickHz.value);
    Println("[kern] keyboard on, type to echo");
    for (;;) {
        asm volatile("hlt" : : : "memory");
        while (keyboard.poll()) {
            PutChar(keyboard.take());
        }
    }
}

}  // namespace kernel
