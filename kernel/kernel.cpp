#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/init/init_sequence.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/time/tick.hpp"
#include "kernel/time/tick_config.hpp"

extern "C" unsigned char g_user_shell_start[];
extern "C" unsigned char g_user_shell_end[];

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

    Println("[kern] launching shell in ring 3");
    cinux::fs::FileWorld::self().init();
    auto launch_shell = +[]() {
        static cinux::mm::AddressSpace shell_space;
        cinux::base::safety::Check(shell_space.init(), "user address space failed");
        constexpr unsigned long kUserCodeBase  = 0x400000;
        constexpr unsigned long kUserStackBase = 0x500000;
        constexpr unsigned long kUserStackTop  = kUserStackBase + 4096;
        auto const              kShellPhys =
            reinterpret_cast<unsigned long>(&g_user_shell_start) - cinux::mm::kKernelImageBase;
        cinux::base::safety::Check(shell_space.map_user(kUserCodeBase, kShellPhys),
                                   "shell code page failed");
        const cinux::base::PhysAddr kStackFrame = cinux::mm::Pmm::self().allocate_page();
        cinux::base::safety::Check(kStackFrame != cinux::base::PhysAddr{},
                                   "user stack page failed");
        cinux::base::safety::Check(shell_space.map_user(kUserStackBase, kStackFrame.raw),
                                   "user stack page failed");
        cinux::proc::Scheduler::self().current()->user_root = shell_space.root();
        asm volatile("cli" : : : "memory");
        shell_space.activate();
        JumpToRing3(kUserCodeBase, kUserStackTop);
    };
    auto* const kShell =
        cinux::proc::TaskBuilder{}.set_entry(launch_shell).set_name("user_shell").build();
    cinux::base::safety::Check(kShell != nullptr, "shell task failed to build");
    cinux::proc::Scheduler::self().seat(*kShell);
    for (;;) {
        cinux::proc::Scheduler::self().run_until_done();
        asm volatile("hlt" : : : "memory");
    }
}

}  // namespace kernel
