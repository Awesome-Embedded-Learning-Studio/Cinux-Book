#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/console.hpp"
#include "kernel/boot/print.hpp"

#ifdef KTEST_STAGE_IRQ
#    include "kernel/arch/x86_64/irq_stubs.hpp"
#    include "kernel/arch/x86_64/pic.hpp"
#    include "kernel/driver/pit.hpp"
#    include "kernel/interrupt/irq.hpp"
#    include "kernel/time/tick.hpp"
#endif

namespace kernel {

// NOLINTNEXTLINE(misc-use-internal-linkage)
void Main(const cinux::boot::BootInfo& info) {
    cinux::console::InitConsole();
    cinux::print::Println("[ktest] kernel test world alive, %u e820 entries", info.e820_count);
#ifdef KTEST_STAGE_IRQ
    cinux::arch::Pic::self().remap();
    cinux::time::Tick::self().init(cinux::driver::Pit::self());
    cinux::arch::irq::InstallIrqStubs();
    cinux::interrupt::Irq::self().enable_line(cinux::interrupt::IrqLine{.value = 0});
    asm volatile("sti" : : : "memory");
#endif
    cinux::test::RunKernelTests();
    cinux::arch::Halt();
}

}  // namespace kernel
