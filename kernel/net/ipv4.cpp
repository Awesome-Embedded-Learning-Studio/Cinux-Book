#include "kernel/net/ipv4.hpp"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

void Ipv4Module::init(const DatagramSink& sink, uint32_t own_address) {
    sink_           = sink;
    own_address_    = own_address;
    stats_          = {};
    protocol_count_ = 0;
}

void Ipv4Module::aim(const DatagramSink& sink, uint32_t own_address) {
    sink_        = sink;
    own_address_ = own_address;
}

bool Ipv4Module::seat_protocol(unsigned char protocol, L4Seat seat) {
    if (protocol_count_ == kMaxL4Seats) {
        return false;
    }
    for (unsigned long index = 0; index < protocol_count_; ++index) {
        if (protocols_[index].protocol == protocol) {
            return false;
        }
    }
    protocols_[protocol_count_] = {.protocol = protocol, .seat = seat};
    ++protocol_count_;
    return true;
}

void Ipv4Module::handle(const unsigned char* bytes, size_t length) {
    const auto kView = ParseIpv4(bytes, length);
    if (!kView.ok()) {
        switch (kView.error()) {
        case Ipv4Error::kDatagramTooShort:
            ++stats_.refused_short;
            break;
        case Ipv4Error::kBadVersion:
            ++stats_.refused_version;
            break;
        case Ipv4Error::kBadHeaderWords:
            ++stats_.refused_header_words;
            break;
        case Ipv4Error::kBadTotalLength:
            ++stats_.refused_total_length;
            break;
        }
        return;
    }

    base::InternetChecksum verifier;
    verifier.add(bytes, kView.value().header_bytes);
    if (verifier.value() != 0) {
        ++stats_.refused_checksum;
        return;
    }

    const size_t kEffective     = std::min(static_cast<size_t>(kView.value().total_length), length);
    const size_t kPayloadLength = kEffective - kView.value().header_bytes;

    for (unsigned long index = 0; index < protocol_count_; ++index) {
        if (protocols_[index].protocol == kView.value().protocol) {
            protocols_[index].seat.handle(bytes + kView.value().header_bytes, kPayloadLength,
                                          kView.value());
            ++stats_.delivered;
            return;
        }
    }
    ++stats_.unknown_protocol;
}

bool Ipv4Module::send(const OutboundDatagram& datagram) {
    if (!sink_.assigned() || datagram.length > kMaxDatagramBytes - kMinIpv4HeaderBytes) {
        return false;
    }

    const unsigned long kTotal = kMinIpv4HeaderBytes + datagram.length;
    outgoing_[0] =
        static_cast<unsigned char>((static_cast<unsigned>(kIpv4Version) << 4U) |
                                   static_cast<unsigned>(kMinIpv4HeaderBytes / sizeof(uint32_t)));
    outgoing_[1] = 0;
    base::WriteWord16Be(outgoing_, 2, static_cast<uint16_t>(kTotal));
    base::WriteWord16Be(outgoing_, 4, 0);
    base::WriteWord16Be(outgoing_, 6, 0);
    outgoing_[8] = kIpv4TimeToLive;
    outgoing_[9] = datagram.protocol;
    base::WriteWord16Be(outgoing_, 10, 0);
    base::WriteWord32Be(outgoing_, 12, own_address_);
    base::WriteWord32Be(outgoing_, 16, datagram.destination);
    for (size_t index = 0; index < datagram.length; ++index) {
        outgoing_[kMinIpv4HeaderBytes + index] = datagram.payload[index];
    }

    base::InternetChecksum builder;
    builder.add(outgoing_, kMinIpv4HeaderBytes);
    base::WriteWord16Be(outgoing_, 10, builder.value());

    const OutboundDatagram kOutbound = {
        .destination = datagram.destination,
        .protocol    = datagram.protocol,
        .payload     = outgoing_,
        .length      = kTotal,
    };
    sink_.send(sink_.context, kOutbound);
    ++stats_.sent;
    return true;
}

const Ipv4Stats& Ipv4Module::stats() const {
    return stats_;
}

}  // namespace cinux::net
