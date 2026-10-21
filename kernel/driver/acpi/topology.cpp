#include "cinux/byte_order.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/acpi/madt.hpp"

namespace cinux::driver::acpi {

InterruptTopology FindInterruptTopology(const Rsdp* root) {
    const auto* const kTable = FindTable(root, "APIC");
    if (kTable == nullptr) {
        return {};
    }
    const auto* const kBytes = reinterpret_cast<const unsigned char*>(kTable);
    return ParseMadt(kBytes, base::ReadWord32(kBytes, 4));
}

}
