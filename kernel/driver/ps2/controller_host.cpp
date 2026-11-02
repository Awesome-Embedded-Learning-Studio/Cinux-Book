#include <cstdint>

#include "cinux/time.hpp"
#include "kernel/driver/base/io.hpp"
#include "kernel/driver/pit/pit.hpp"
#include "kernel/driver/ps2/controller.hpp"

namespace cinux::driver::ps2 {

namespace {

using ps2::kDataPort;

constexpr uint16_t kCommandPort = 0x64;

uint8_t host_status(void* /*context*/) {
    return driver::InB(kCommandPort);
}

uint8_t host_data_in(void* /*context*/) {
    return driver::InB(ps2::kDataPort);
}

void host_command_out(void* /*context*/, uint8_t value) {
    driver::OutB({.port = kCommandPort, .value = value});
}

void host_data_out(void* /*context*/, uint8_t value) {
    driver::OutB({.port = kDataPort, .value = value});
}

// NOLINTNEXTLINE(misc-const-correctness) injected transport callback signature.
void host_delay([[maybe_unused]] void* context, base::time::Microseconds span) {
    Pit::self().wait_microseconds(span);
}

}  // namespace

Ps2Ports HostPs2Ports() {
    return {
        .context     = nullptr,
        .status      = host_status,
        .data_in     = host_data_in,
        .command_out = host_command_out,
        .data_out    = host_data_out,
        .delay       = host_delay,
    };
}

}  // namespace cinux::driver::ps2
