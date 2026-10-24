#include "kernel/net/ifaces.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_world.hpp"

namespace cinux::net {

namespace {

/// The loopback's mask: the top byte alone, its network's own shape.
constexpr uint32_t kLoopbackMask = 0xFF000000;

/// The wired network's mask: the usual twenty-four bits, 255.255.255.0.
constexpr uint32_t kWireMask = 0xFFFFFF00;

/// The loopback answers no hardware size limit of its own.
constexpr uint32_t kLoopbackMtu = 65536;

/// The wire carries the family's frame ceiling.
constexpr uint32_t kWireMtu = 1500;

void copy_name(char* out, const char* name) {
    unsigned long index = 0;
    while (name[index] != '\0' && index < kIfNameMax - 1) {
        out[index] = name[index];
        ++index;
    }
    out[index] = '\0';
}

bool wire_facts(InterfaceFacts& into) {
    const NetWorld& world = NetWorld::self();
    if (!world.wired()) {
        return false;
    }
    into.address       = kUserNetworkSelf;
    into.netmask       = kWireMask;
    into.broadcast     = (into.address & into.netmask) | ~into.netmask;
    into.hardware_type = kHardwareEthernet;
    into.mtu           = kWireMtu;
    into.hardware      = world.wire()->mac();
    return true;
}

}  // namespace

bool ReadInterfaceFacts(const char* name, InterfaceFacts& into) {
    if (name == nullptr) {
        return false;
    }
    if (name[0] == kLoopbackName[0] && name[1] == kLoopbackName[1] && name[2] == '\0') {
        into               = InterfaceFacts{};
        into.address       = kLoopbackAddress;
        into.netmask       = kLoopbackMask;
        into.broadcast     = kLoopbackAddress;
        into.hardware_type = kHardwareLoopback;
        into.mtu           = kLoopbackMtu;
        return true;
    }
    if (name[0] == kWireName[0] && name[1] == kWireName[1] && name[2] == kWireName[2] &&
        name[3] == kWireName[3] && name[4] == '\0') {
        into = InterfaceFacts{};
        return wire_facts(into);
    }
    return false;
}

bool NameInterfaceAt(unsigned long ordinal, char* out) {
    if (out == nullptr) {
        return false;
    }
    if (ordinal == 0) {
        copy_name(out, kLoopbackName);
        return true;
    }
    if (ordinal == 1 && NetWorld::self().wired()) {
        copy_name(out, kWireName);
        return true;
    }
    return false;
}

unsigned long InterfaceCount() {
    return NetWorld::self().wired() ? 2 : 1;
}

}  // namespace cinux::net
