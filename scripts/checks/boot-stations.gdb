python
import gdb

stations = [
    "KernelEntry",
    "KernelStart",
    "kernel::Main",
    "cinux::init::RunInitSequence",
    "cinux::driver::SerialInit",
    "cinux::mm::Pmm::init",
    "cinux::mm::BringUpAddressSpace",
    "cinux::proc::InstallFaultResolver",
    "cinux::console::TextConsole::init",
    "cinux::mm::BringUpHeap",
    "cinux::driver::LocalApic::init",
    "cinux::arch::Pic::remap",
    "cinux::arch::usermode::EnableFastSystemCalls",
    "cinux::arch::SeedKernelRandom",
    "cinux::arch::EnableCoreProtections",
    "cinux::time::Tick::init",
    "cinux::arch::irq::InstallIrqStubs",
    "cinux::arch::irq::InstallApicBackend",
    "cinux::interrupt::Irq::enable_line",
    "cinux::driver::Keyboard::init",
    "cinux::driver::Ps2Mouse::init",
    "cinux::driver::PciBus::init",
    "cinux::driver::Ahci::init",
    "cinux::driver::Hpet::init",
    "cinux::driver::Nvme::init",
    "cinux::driver::XhciHost::init",
    "cinux::driver::UsbKeyboard::init",
    "cinux::driver::net::Loopback::init",
    "cinux::arch::smp::BootSecondaryCpus",
    "cinux::proc::InstallKernelSwitchSink",
    "cinux::init::StartInitThread",
    "cinux::arch::smp::EnableSecondaryDispatch",
]

last = "long-mode handoff"
for name in stations:
    try:
        gdb.execute("delete breakpoints")
        gdb.execute("hbreak '%s'" % name)
        gdb.execute("continue")
    except gdb.error as error:
        print("BOOT STOPPED BEFORE: %s (%s)" % (name, error))
        break
    last = name
    print("BOOT REACHED: %s" % name)
print("BOOT LAST STATION: %s" % last)

if last == stations[-1]:
    print("BOOT ALL STATIONS PASSED, RECORDING SINGLE STEPS")
    steps = []
    try:
        for _ in range(20000):
            gdb.execute("stepi", to_string=True)
            steps.append(str(gdb.parse_and_eval("$pc")))
    except gdb.error as error:
        print("BOOT STEP TRACE ENDED: %s" % error)
    print("BOOT STEP COUNT: %d" % len(steps))
    for pc in steps[-40:]:
        print("BOOT STEP PC: %s" % pc)
end
