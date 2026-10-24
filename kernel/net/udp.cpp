#include "kernel/net/udp.hpp"

#include <stddef.h>
#include <stdint.h>

#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

namespace {

/**
 * @brief   The pseudo header's fixed shape: twelve bytes, three facts.
 * @note    Source and destination addresses, the protocol word, and the
 *          UDP length the checksum must sum ahead of the datagram. Kept
 *          as byte counts so the write words stay plain.
 * @since   0.7.0
 * @ingroup kernel_net
 */
constexpr unsigned long kPseudoHeaderBytes = 12;

void build_pseudo_header(unsigned char* pseudo, uint32_t source, uint32_t destination,
                         uint16_t datagram_bytes) {
    base::WriteWord32Be(pseudo, 0, source);
    base::WriteWord32Be(pseudo, 4, destination);
    pseudo[8] = 0;
    pseudo[9] = kIpv4ProtocolUdp;
    base::WriteWord16Be(pseudo, 10, datagram_bytes);
}

}  // namespace

void UdpModule::init(const DatagramSink& sink, uint32_t own_address) {
    sink_        = sink;
    own_address_ = own_address;
    stats_       = {};
}

void UdpModule::aim(const DatagramSink& sink, uint32_t own_address) {
    sink_        = sink;
    own_address_ = own_address;
}

void UdpModule::handle(const unsigned char* bytes, size_t length, const Ipv4View& header) {
    const auto kView = ParseUdp(bytes, length);
    if (!kView.ok()) {
        if (kView.error() == UdpError::kDatagramTooShort) {
            ++stats_.refused_short;
        } else {
            ++stats_.refused_length;
        }
        return;
    }
    if (kView.value().length < kUdpHeaderBytes || kView.value().length > length) {
        ++stats_.refused_length;
        return;
    }

    if (kView.value().checksum != 0) {
        unsigned char pseudo[kPseudoHeaderBytes] = {};
        build_pseudo_header(pseudo, header.source, header.destination, kView.value().length);
        base::InternetChecksum verifier;
        verifier.add(pseudo, sizeof(pseudo));
        verifier.add(bytes, kView.value().length);
        if (verifier.value() != 0) {
            ++stats_.refused_checksum;
            return;
        }
    }

    const auto kPorts = ports_gate_.guard();
    for (unsigned long index = 0; index < port_count_; ++index) {
        if (ports_[index].port == kView.value().destination_port) {
            const UdpDelivery kDelivery = {
                .sender      = header.source,
                .sender_port = kView.value().source_port,
                .bytes       = bytes + kUdpHeaderBytes,
                .length      = static_cast<size_t>(kView.value().length) - kUdpHeaderBytes,
            };
            ports_[index].deliver(ports_[index].context, kDelivery);
            ++stats_.delivered;
            return;
        }
    }
    ++stats_.unclaimed;
}

// NOLINTNEXTLINE(readability-function-size) a UDP send names five facts
bool UdpModule::send(uint32_t destination, uint16_t source_port, uint16_t destination_port,
                     const unsigned char* payload, size_t length) {
    if (length > kMaxDatagramBytes - kMinIpv4HeaderBytes - kUdpHeaderBytes) {
        return false;
    }
    unsigned char outgoing[kMaxDatagramBytes]{};
    const auto    kDatagramBytes = static_cast<uint16_t>(kUdpHeaderBytes + length);
    base::WriteWord16Be(outgoing, 0, source_port);
    base::WriteWord16Be(outgoing, 2, destination_port);
    base::WriteWord16Be(outgoing, 4, kDatagramBytes);
    base::WriteWord16Be(outgoing, 6, 0);
    for (size_t index = 0; index < length; ++index) {
        outgoing[kUdpHeaderBytes + index] = payload[index];
    }

    unsigned char pseudo[kPseudoHeaderBytes] = {};
    build_pseudo_header(pseudo, own_address_, destination, kDatagramBytes);
    base::InternetChecksum builder;
    builder.add(pseudo, sizeof(pseudo));
    builder.add(outgoing, kDatagramBytes);
    uint16_t checksum = builder.value();
    if (checksum == 0) {
        checksum = 0xFFFF;
    }
    base::WriteWord16Be(outgoing, 6, checksum);

    if (!sink_.assigned()) {
        return false;
    }
    const OutboundDatagram kDatagram = {
        .destination = destination,
        .protocol    = kIpv4ProtocolUdp,
        .payload     = outgoing,
        .length      = kDatagramBytes,
    };
    sink_.send(sink_.context, kDatagram);
    ++stats_.sent;
    return true;
}

bool UdpModule::bind(uint16_t port, void* context, UdpDeliver deliver) {
    const auto kPorts = ports_gate_.guard();
    if (deliver == nullptr) {
        return false;
    }
    if (port_count_ == kUdpPortSeats) {
        return false;
    }
    for (unsigned long index = 0; index < port_count_; ++index) {
        if (ports_[index].port == port) {
            return false;
        }
    }
    ports_[port_count_] = {.port = port, .context = context, .deliver = deliver};
    ++port_count_;
    return true;
}

bool UdpModule::unbind(uint16_t port) {
    const auto kPorts = ports_gate_.guard();
    for (unsigned long index = 0; index < port_count_; ++index) {
        if (ports_[index].port == port) {
            ports_[index] = ports_[port_count_ - 1];
            --port_count_;
            return true;
        }
    }
    return false;
}

uint16_t UdpModule::ephemeral_port() const {
    const auto kPorts = ports_gate_.guard();
    for (uint32_t scan = 0; scan <= kUdpEphemeralLast - kUdpEphemeralFirst; ++scan) {
        const auto kCandidate = static_cast<uint16_t>(
            kUdpEphemeralFirst + ((ephemeral_cursor_ - kUdpEphemeralFirst + scan) %
                                  (kUdpEphemeralLast - kUdpEphemeralFirst + 1)));
        bool taken = false;
        for (unsigned long index = 0; index < port_count_; ++index) {
            if (ports_[index].port == kCandidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return kCandidate;
        }
    }
    return 0;
}

const UdpStats& UdpModule::stats() const {
    return stats_;
}

uint32_t UdpModule::own_address() const {
    return own_address_;
}

}  // namespace cinux::net
