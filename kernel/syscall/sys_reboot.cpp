#include "kernel/syscall/sys_reboot.hpp"

#include "kernel/console/console.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

long long HandleReboot(const SyscallFrame& /*frame*/) {
    const auto* const kFadt = driver::acpi::FindFadt(driver::acpi::FindRootPointer());
    if (kFadt == nullptr) {
        return -kEnosys;
    }
    console::Flush();
    driver::acpi::PowerOff(kFadt);
    return 0;
}

}  // namespace cinux::syscall
