#include "kernel/syscall/sys_reboot.hpp"

#include "kernel/console/console.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleReboot(const SyscallFrame& frame) {
    if (CurrentTask()->euid != 0) {
        return -kEperm;
    }
    if (frame.rdi != kRebootMagic1 || frame.rsi != kRebootMagic2) {
        return -kEinval;
    }
    if (frame.rdx == 0) {
        return 0;
    }
    if (frame.rdx != kRebootPowerOff) {
        return -kEinval;
    }
    const auto* const kFadt = driver::acpi::FindFadt(driver::acpi::FindRootPointer());
    if (kFadt == nullptr) {
        return -kEnosys;
    }
    if (!fs::FileWorld::self().sync()) {
        return -kEio;
    }
    console::Flush();
    driver::acpi::PowerOff(kFadt);
    return 0;
}

}  // namespace cinux::syscall
