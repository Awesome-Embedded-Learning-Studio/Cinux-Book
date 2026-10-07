#include "kernel/init/init_sequence.hpp"

#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/irq_stubs.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/keyboard.hpp"
#include "kernel/driver/pit.hpp"
#include "kernel/driver/serial.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/mm/heap_runtime.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/time/tick.hpp"

namespace cinux::init {

namespace {

using boot::BootInfo;

constexpr InitStep kInitSteps[] = {
    {.name = "serial",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         driver::SerialInit();
         return info;
     }},
    {.name = "pmm",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         return mm::Pmm::self().init(*info) ? info : nullptr;
     }},
    {.name = "address space",
     .run =
         +[](const BootInfo* info) -> const BootInfo* { return mm::BringUpAddressSpace(*info); }},
    {.name = "console",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         console::TextConsole::self().init(info->framebuffer);
         return info;
     }},
    {.name = "heap",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         return mm::BringUpHeap() ? info : nullptr;
     }},
    {.name = "pic",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         arch::Pic::self().remap();
         return info;
     }},
    {.name = "fast system calls",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         arch::usermode::EnableFastSystemCalls();
         return info;
     }},
    {.name = "tick",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         time::Tick::self().init(driver::Pit::self());
         return info;
     }},
    {.name = "irq stubs",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         arch::irq::InstallIrqStubs();
         return info;
     }},
    {.name = "irq line 0",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         interrupt::Irq::self().enable_line(interrupt::IrqLine{.value = 0});
         return info;
     }},
    {.name = "keyboard",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         if (!driver::Keyboard::self().init()) {
             return nullptr;
         }
         driver::Keyboard::self().attach();
         return info;
     }},
    {.name = "interrupts",
     .run  = +[](const BootInfo* info) -> const BootInfo* {
         asm volatile("sti" : : : "memory");
         return info;
     }},
};

}  // namespace

const boot::BootInfo* RunInitSequence(const boot::BootInfo* info) {
    for (const InitStep& step : kInitSteps) {
        auto const* const kNext = step.run(info);
        if (kNext == nullptr) {
            print::Println("[kern] init '%s' failed, halting", step.name);
            arch::Halt();
        }
        info = kNext;
    }
    return info;
}

}  // namespace cinux::init
