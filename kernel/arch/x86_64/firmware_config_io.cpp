#include <cstdint>
#include <span>
#include <string_view>

#include "kernel/arch/x86_64/firmware_config.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/base/io.hpp"

namespace cinux::arch {
namespace {
void select_item(void* /*context*/, std::uint16_t selector) {
    driver::Out16({.port = 0x510, .value = selector});
}
unsigned char read_byte(void* /*context*/) {
    return driver::InB(0x511);
}
}
bool ReadFirmwareString(std::string_view name, std::span<char> output) {
    const IrqGuard kGuard;
    return ReadFirmwareString({.select = select_item, .read = read_byte}, name, output);
}
}
