#include "kernel/net/raw.hpp"

#include <cstddef>
#include <cstdint>

#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

bool RawModule::bind(void* context, Receive receive) {
    const auto kGuard = gate_.guard();
    for (auto& listener : listeners_) {
        if (listener.context == nullptr) {
            listener = {.context = context, .receive = receive};
            return true;
        }
    }
    return false;
}

void RawModule::unbind(void* context) {
    const auto kGuard = gate_.guard();
    for (auto& listener : listeners_) {
        if (listener.context == context) {
            listener = {};
        }
    }
}

void RawModule::handle(const unsigned char* bytes, size_t length, const Ipv4View& header) {
    if (header.protocol != kIpv4ProtocolIcmp || length > kMaxDatagramBytes) {
        return;
    }
    const auto kGuard = gate_.guard();
    for (const auto& listener : listeners_) {
        if (listener.context != nullptr) {
            listener.receive(listener.context, bytes, length, header);
        }
    }
}

bool RawModule::send(uint32_t destination, const unsigned char* bytes, size_t length) {
    return ipv4_.send({
        .destination = destination,
        .protocol    = kIpv4ProtocolIcmp,
        .payload     = bytes,
        .length      = length,
    });
}

}
