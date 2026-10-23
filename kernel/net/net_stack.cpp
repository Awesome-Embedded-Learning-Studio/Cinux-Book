#include "kernel/net/net_stack.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

void NetStack::init() {
    stats_          = {};
    port_count_     = 0;
    protocol_count_ = 0;
}

bool NetStack::attach_port(driver::net::NetPort& port) {
    if (port_count_ == kMaxPorts) {
        return false;
    }
    ports_[port_count_] = &port;
    ++port_count_;
    return true;
}

bool NetStack::seat_protocol(uint16_t ethertype, ProtocolSeat seat) {
    if (protocol_count_ == kMaxProtocolSeats) {
        return false;
    }
    for (unsigned long index = 0; index < protocol_count_; ++index) {
        if (protocols_[index].ethertype == ethertype) {
            return false;
        }
    }
    protocols_[protocol_count_] = {.ethertype = ethertype, .seat = seat};
    ++protocol_count_;
    return true;
}

void NetStack::poll(unsigned long budget) {
    for (unsigned long index = 0; index < port_count_; ++index) {
        driver::net::RxFrame frame = {};
        while (budget != 0 && ports_[index]->poll_frame(frame)) {
            --budget;
            dispatch(*ports_[index], frame);
        }
    }
}

const NetStackStats& NetStack::stats() const {
    return stats_;
}

void NetStack::dispatch(const driver::net::NetPort& port, const driver::net::RxFrame& frame) {
    const unsigned char* payload   = frame.bytes;
    size_t               length    = frame.size;
    uint16_t             ethertype = kEtherTypeIpv4;

    if (port.has_ethernet_header()) {
        const auto kHeader = ParseEthernetHeader(frame.bytes, frame.size);
        if (!kHeader.ok()) {
            ++stats_.refused_ethernet;
            return;
        }
        ethertype = kHeader.value().ethertype;
        payload   = frame.bytes + kEthernetHeaderBytes;
        length    = frame.size - kEthernetHeaderBytes;
    }

    for (unsigned long index = 0; index < protocol_count_; ++index) {
        if (protocols_[index].ethertype == ethertype) {
            protocols_[index].seat.handle(payload, length);
            ++stats_.delivered;
            return;
        }
    }
    ++stats_.unknown_protocol;
}

}  // namespace cinux::net
