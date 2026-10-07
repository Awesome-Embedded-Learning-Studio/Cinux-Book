#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
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
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
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

    auto cooperative_echo = +[]() {
        auto& scheduler = cinux::proc::Scheduler::self();
        for (unsigned int round = 0; round < 3; ++round) {
            Println("[task] tid=%u '%s' round %u", scheduler.current()->tid,
                    scheduler.current()->name, round);
            scheduler.yield();
        }
    };
    cinux::proc::InstallKernelSwitchSink();
    auto* const kEchoA =
        cinux::proc::TaskBuilder{}.set_entry(cooperative_echo).set_name("echo_a").build();
    auto* const kEchoB =
        cinux::proc::TaskBuilder{}.set_entry(cooperative_echo).set_name("echo_b").build();
    cinux::base::safety::Check(kEchoA != nullptr && kEchoB != nullptr,
                               "echo tasks failed to build");
    cinux::proc::Scheduler::self().seat(*kEchoA);
    cinux::proc::Scheduler::self().seat(*kEchoB);
    cinux::proc::Scheduler::self().run_until_done();
    Println("[kern] cooperative tasks drained, back on main");

    auto preempt_echo = +[]() {
        auto& scheduler = cinux::proc::Scheduler::self();
        for (unsigned int round = 0; round < 3; ++round) {
            const unsigned long long kDeadline = cinux::time::Tick::self().since_boot() + 15;
            while (cinux::time::Tick::self().since_boot() < kDeadline) {
            }
            Println("[task] tid=%u '%s' round %u (no yield)", scheduler.current()->tid,
                    scheduler.current()->name, round);
        }
    };
    auto* const kSpinA =
        cinux::proc::TaskBuilder{}.set_entry(preempt_echo).set_name("spin_a").build();
    auto* const kSpinB =
        cinux::proc::TaskBuilder{}.set_entry(preempt_echo).set_name("spin_b").build();
    cinux::base::safety::Check(kSpinA != nullptr && kSpinB != nullptr,
                               "spin tasks failed to build");
    cinux::proc::Scheduler::self().seat(*kSpinA);
    cinux::proc::Scheduler::self().seat(*kSpinB);
    cinux::proc::Scheduler::self().set_preemption(true);
    cinux::proc::Scheduler::self().run_until_done();

    struct Market {
        cinux::proc::Mutex     door;
        cinux::proc::Semaphore free_slots;
        cinux::proc::Semaphore goods;
        unsigned int           buffer[4]  = {};
        unsigned int           put_index  = 0;
        unsigned int           take_index = 0;
    };
    static Market market;
    for (unsigned int slot = 0; slot < 4; ++slot) {
        market.free_slots.post();
    }
    auto produce = +[]() {
        for (unsigned int item = 1; item <= 6; ++item) {
            market.free_slots.wait();
            {
                const cinux::proc::MutexGuard kDoor(market.door);
                market.buffer[market.put_index % 4] = item;
                market.put_index++;
            }
            market.goods.post();
            Println("[task] tid=%u produced item %u", cinux::proc::Scheduler::self().current()->tid,
                    item);
        }
    };
    auto consume = +[]() {
        for (unsigned int round = 0; round < 6; ++round) {
            market.goods.wait();
            unsigned int item = 0;
            {
                const cinux::proc::MutexGuard kDoor(market.door);
                item = market.buffer[market.take_index % 4];
                market.take_index++;
            }
            market.free_slots.post();
            Println("[task] tid=%u consumed item %u", cinux::proc::Scheduler::self().current()->tid,
                    item);
        }
    };
    auto* const kProducer =
        cinux::proc::TaskBuilder{}.set_entry(produce).set_name("producer").build();
    auto* const kConsumer =
        cinux::proc::TaskBuilder{}.set_entry(consume).set_name("consumer").build();
    cinux::base::safety::Check(kProducer != nullptr && kConsumer != nullptr,
                               "market tasks failed to build");
    cinux::proc::Scheduler::self().seat(*kProducer);
    cinux::proc::Scheduler::self().seat(*kConsumer);
    cinux::proc::Scheduler::self().run_until_done();
    Println("[kern] preemptive tasks drained without a single yield");
    Println("[kern] market closed the loop, preemption stays armed");

    for (;;) {
        asm volatile("hlt" : : : "memory");
        while (keyboard.poll()) {
            PutChar(keyboard.take());
        }
    }
}

}  // namespace kernel
