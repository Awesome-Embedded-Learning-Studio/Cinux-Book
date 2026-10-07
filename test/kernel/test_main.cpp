#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/console/console.hpp"
#include "kernel/mm/vmm.hpp"

#if defined(KTEST_STAGE_IRQ) || defined(KTEST_STAGE_KEYBOARD) || defined(KTEST_STAGE_PROC)
#    include "kernel/arch/x86_64/irq_stubs.hpp"
#    include "kernel/arch/x86_64/pic.hpp"
#    include "kernel/driver/pit.hpp"
#    include "kernel/interrupt/irq.hpp"
#    include "kernel/time/tick.hpp"
#endif
#ifdef KTEST_STAGE_KEYBOARD
#    include "kernel/driver/keyboard.hpp"
#endif
#if defined(KTEST_STAGE_MM) || defined(KTEST_STAGE_PROC)
#    include "kernel/driver/serial.hpp"
#    include "kernel/mm/heap_runtime.hpp"
#    include "kernel/mm/pmm.hpp"
#endif
#ifdef KTEST_STAGE_PROC
#    include "kernel/proc/task.hpp"
#endif

namespace kernel {

// NOLINTNEXTLINE(misc-use-internal-linkage)
void Main(const cinux::boot::BootInfo& info) {
#if defined(KTEST_STAGE_MM) || defined(KTEST_STAGE_PROC)
    cinux::driver::SerialInit();
    if (!cinux::mm::Pmm::self().init(info)) {
        cinux::print::Println("[ktest] pmm init failed");
        cinux::arch::Halt();
    }
    const cinux::boot::BootInfo* handed = cinux::mm::BringUpAddressSpace(info);
    if (handed == nullptr) {
        cinux::print::Println("[ktest] address space bring-up failed");
        cinux::arch::Halt();
    }
    cinux::console::InitConsole(*handed);
    if (!cinux::mm::BringUpHeap()) {
        cinux::print::Println("[ktest] heap bring-up failed");
        cinux::arch::Halt();
    }
#else
    cinux::mm::MapFramebufferDoor(info);
    cinux::console::InitConsole(info);
#endif
    cinux::print::Println("[ktest] kernel test world alive, %u e820 entries", info.e820_count);
#ifdef KTEST_STAGE_PROC
    cinux::proc::InstallKernelSwitchSink();
#endif
#if defined(KTEST_STAGE_IRQ) || defined(KTEST_STAGE_KEYBOARD)
    cinux::arch::Pic::self().remap();
    cinux::time::Tick::self().init(cinux::driver::Pit::self());
    cinux::arch::irq::InstallIrqStubs();
    cinux::interrupt::Irq::self().enable_line(cinux::interrupt::IrqLine{.value = 0});
#endif
#ifdef KTEST_STAGE_PROC
    cinux::arch::Pic::self().remap();
    cinux::time::Tick::self().init(cinux::driver::Pit::self());
    cinux::arch::irq::InstallIrqStubs();
    cinux::interrupt::Irq::self().enable_line(cinux::interrupt::IrqLine{.value = 0});
    cinux::print::Println("[ktest] proc stage: irq base on, timer live");
#endif
#ifdef KTEST_STAGE_KEYBOARD
    cinux::driver::Keyboard::self().init();
    cinux::driver::Keyboard::self().attach();
    cinux::print::Println("[ktest] keyboard attached");
#endif
#if defined(KTEST_STAGE_IRQ) || defined(KTEST_STAGE_KEYBOARD) || defined(KTEST_STAGE_PROC)
    asm volatile("sti" : : : "memory");
#endif
    cinux::test::RunKernelTests();
    cinux::arch::Halt();
}

}  // namespace kernel
