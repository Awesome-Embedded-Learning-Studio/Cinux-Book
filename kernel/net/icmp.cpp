#include "kernel/net/icmp.hpp"

#include <stddef.h>
#include <stdint.h>

#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

void IcmpModule::init(const DatagramSink& reply_sink) {
    reply_sink_ = reply_sink;
    stats_      = {};
}

void IcmpModule::handle(const unsigned char* bytes, size_t length, const Ipv4View& header) {
    const auto kView = ParseIcmpEcho(bytes, length);
    if (!kView.ok()) {
        ++stats_.refused_short;
        return;
    }
    if (length > kMaxIcmpEchoBytes) {
        ++stats_.refused_oversized;
        return;
    }

    base::InternetChecksum verifier;
    verifier.add(bytes, length);
    if (verifier.value() != 0) {
        ++stats_.refused_checksum;
        return;
    }

    if (kView.value().type == kIcmpEchoRequest) {
        reply_[0] = kIcmpEchoReply;
        reply_[1] = 0;
        base::WriteWord16Be(reply_, 2, 0);
        base::WriteWord16Be(reply_, 4, kView.value().identifier);
        base::WriteWord16Be(reply_, 6, kView.value().sequence);
        for (size_t index = kIcmpEchoHeaderBytes; index < length; ++index) {
            reply_[index] = bytes[index];
        }

        base::InternetChecksum builder;
        builder.add(reply_, length);
        base::WriteWord16Be(reply_, 2, builder.value());

        if (reply_sink_.assigned()) {
            const OutboundDatagram kReply = {
                .destination = header.source,
                .protocol    = kIpv4ProtocolIcmp,
                .payload     = reply_,
                .length      = length,
            };
            reply_sink_.send(reply_sink_.context, kReply);
        }
        ++stats_.requests_answered;
        return;
    }

    if (kView.value().type == kIcmpEchoReply) {
        ++stats_.replies_seen;
        stats_.last_identifier = kView.value().identifier;
        stats_.last_sequence   = kView.value().sequence;
    }
}

const IcmpStats& IcmpModule::stats() const {
    return stats_;
}

void BuildEchoRequest(unsigned char* message, size_t length, uint16_t identifier,
                      uint16_t sequence) {
    message[0] = kIcmpEchoRequest;
    message[1] = 0;
    base::WriteWord16Be(message, 2, 0);
    base::WriteWord16Be(message, 4, identifier);
    base::WriteWord16Be(message, 6, sequence);
    for (size_t index = kIcmpEchoHeaderBytes; index < length; ++index) {
        message[index] = static_cast<unsigned char>(index);
    }
    base::InternetChecksum checksum;
    checksum.add(message, length);
    base::WriteWord16Be(message, 2, checksum.value());
}

}  // namespace cinux::net
