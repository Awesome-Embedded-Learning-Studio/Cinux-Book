#include "kernel/init/init_sequence.hpp"

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/cpuid.hpp"
#include "kernel/arch/x86_64/entropy.hpp"
#include "kernel/arch/x86_64/ex_table.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/irq_stubs.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/arch/x86_64/protections.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/apic/local_apic.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/hpet/hpet.hpp"
#include "kernel/driver/keyboard/keyboard.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/pci/pci.hpp"
#include "kernel/driver/pit/pit.hpp"
#include "kernel/driver/serial/serial.hpp"
#include "kernel/driver/usb/usb_keyboard.hpp"
#include "kernel/driver/usb/xhci.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/mm/heap_runtime.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/fault_diagnose.hpp"
#include "kernel/proc/fault_resolve.hpp"
#include "kernel/proc/lockdep.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "kernel/time/tick.hpp"

namespace cinux::init {

namespace {

using boot::BootInfo;

constexpr InitStep kInitSteps[] = {
    {
        .name = "serial",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            driver::SerialInit();
            return info;
        },
    },
    {
        .name = "pmm",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            return mm::Pmm::self().init(*info) ? info : nullptr;
        },
    },
    {
        .name = "address space",
        .run =
            +[](const BootInfo* info) -> const BootInfo* { return mm::BringUpAddressSpace(*info); },
    },
    {
        .name = "page fault resolver",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            proc::InstallFaultResolver();
            proc::InstallFaultDiagnoser();
            proc::InstallLockdepSink(+[](const char* label, unsigned long long address) {
                if (label != nullptr) {
                    print::Println("[lockdep] held: %s", label);
                } else {
                    print::Println("[lockdep] held: lock@%llx", address);
                }
            });
            return info;
        },
    },
    {
        .name = "console",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            console::TextConsole::self().init(info->framebuffer);
            return info;
        },
    },
    {
        .name = "heap",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            return mm::BringUpHeap() ? info : nullptr;
        },
    },
    {
        .name = "local apic mapping",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            const auto kTopology =
                driver::acpi::FindInterruptTopology(driver::acpi::FindRootPointer());
            if (!driver::LocalApic::self().init(kTopology.local_apic_address)) {
                return nullptr;
            }
            print::Println("[apic] %u processor(s), local id %u, IOAPIC %x, overrides %u",
                           kTopology.processor_count, driver::LocalApic::self().id(),
                           kTopology.io_apic_address, kTopology.override_count);
            return info;
        },
    },
    {
        .name = "pic",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            arch::Pic::self().remap();
            return info;
        },
    },
    {
        .name = "fast system calls",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            arch::usermode::EnableFastSystemCalls();
            arch::ex_table::ArmTable();
            syscall::uaccess::ArmWindows(arch::HasSmap());
            return info;
        },
    },
    {
        .name = "kernel random",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            arch::SeedKernelRandom();
            return info;
        },
    },
    {
        .name = "core protections",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            arch::EnableCoreProtections();
            print::Println("[kern] cr4 %X after core protections", arch::ReadCr4());
            return info;
        },
    },
    {
        .name = "tick",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            time::Tick::self().init(driver::Pit::self());
            return info;
        },
    },
    {
        .name = "irq stubs",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            arch::irq::InstallIrqStubs();
            return info;
        },
    },
    {
        .name = "apic interrupt delivery",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            return arch::irq::InstallApicBackend() ? info : nullptr;
        },
    },
    {
        .name = "irq line 0",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            interrupt::Irq::self().enable_line(interrupt::IrqLine{.value = 0});
            return info;
        },
    },
    {
        .name = "keyboard",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            if (!driver::Keyboard::self().init()) {
                return nullptr;
            }
            driver::Keyboard::self().attach();
            return info;
        },
    },
    {
        .name = "pci",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            driver::PciBus::self().init();
            return info;
        },
    },
    {
        .name = "interrupts",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            asm volatile("sti" : : : "memory");
            return info;
        },
    },
    {
        .name = "ahci",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            driver::Ahci::self().init();
            return info;
        },
    },
    {
        .name = "hpet",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            if (!driver::Hpet::self().init(driver::acpi::FindRootPointer())) {
                print::Println("[kern] hpet absent, clocks stay tick-grained");
            }
            return info;
        },
    },
    {
        .name = "xhci host",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            if (!driver::XhciHost::self().init()) {
                print::Println("[kern] xhci absent, usb stays dark");
            }
            return info;
        },
    },
    {
        .name = "usb keyboard",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            if (!driver::UsbKeyboard::self().init() && driver::XhciHost::self().present()) {
                print::Println("[kern] usb keyboard refused enumeration, ps/2 carries the typing");
            }
            return info;
        },
    },
    {
        .name = "network loopback",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            driver::net::Loopback::self().init();
            const base::Result<void> kLo = driver::DeviceModel::self().seat({
                .klass       = driver::DeviceClass::kNetwork,
                .name        = "lo",
                .number      = 0,
                .ops         = nullptr,
                .bus         = "virtual",
                .driver_name = "loopback",
            });
            base::safety::Check(kLo.ok(), "device model refused the loopback seat");
            return info;
        },
    },
    {
        .name = "secondary processors",
        .run  = +[](const BootInfo* info) -> const BootInfo* {
            if (!arch::smp::BootSecondaryCpus()) {
                return nullptr;
            }
            print::Println("[smp] %u CPUs online", arch::smp::OnlineCpuCount());
            for (unsigned int cpu = 0; cpu < arch::smp::OnlineCpuCount(); ++cpu) {
                const auto& state = arch::smp::Mechanisms(cpu);
                print::Println("[cpu %u] APIC %u online, LSTAR %llx, EFER %llx, CR4 %llx",
                               state.cpu_id, state.apic_id, state.lstar, state.efer, state.cr4);
            }
            return info;
        },
    },
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
