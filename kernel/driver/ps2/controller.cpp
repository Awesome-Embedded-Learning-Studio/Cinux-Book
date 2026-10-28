#include "kernel/driver/ps2/controller.hpp"

#include <cstdint>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/time.hpp"

namespace cinux::driver::ps2 {

namespace {

constexpr uint8_t kStatusOutputFull = 0x01;
constexpr uint8_t kStatusInputFull  = 0x02;

constexpr uint8_t kReplyAcknowledge = 0xFA;
constexpr uint8_t kReplyResend      = 0xFE;

constexpr unsigned long long       kReplyMicroseconds = 200000;
constexpr base::time::Microseconds kPollDelay{50};
constexpr unsigned long            kResendBudget = 2;

bool status_has(uint8_t word, uint8_t bit) {
    return base::bit::BitMask{word}.has(base::bit::BitMask<uint8_t>{bit});
}
bool await_status(Ps2Ports ports, uint8_t bit, bool wanted) {
    for (unsigned long long waited = 0; waited <= kReplyMicroseconds; waited += kPollDelay.value) {
        if (status_has(ports.status(ports.context), bit) == wanted) {
            return true;
        }
        if (waited != kReplyMicroseconds) {
            ports.delay(ports.context, kPollDelay);
        }
    }
    return false;
}

}  // namespace

bool SendCommand(Ps2Ports ports, uint8_t command) {
    if (!await_status(ports, kStatusInputFull, false)) {
        return false;
    }
    ports.command_out(ports.context, command);
    return true;
}
bool WriteData(Ps2Ports ports, uint8_t value) {
    if (!await_status(ports, kStatusInputFull, false)) {
        return false;
    }
    ports.data_out(ports.context, value);
    return true;
}
bool ReadWhenReady(Ps2Ports ports, uint8_t& out) {
    if (!await_status(ports, kStatusOutputFull, true)) {
        return false;
    }
    out = ports.data_in(ports.context);
    return true;
}
bool Drain(Ps2Ports ports) {
    for (unsigned long long waited = 0; waited <= kReplyMicroseconds; waited += kPollDelay.value) {
        if (!status_has(ports.status(ports.context), kStatusOutputFull)) {
            return true;
        }
        ports.data_in(ports.context);
        if (waited != kReplyMicroseconds) {
            ports.delay(ports.context, kPollDelay);
        }
    }
    return false;
}

bool SendAux(Ps2Ports ports, uint8_t value) {
    for (unsigned long attempt = 0; attempt <= kResendBudget; ++attempt) {
        if (!SendCommand(ports, kCommandWriteAux) || !WriteData(ports, value)) {
            return false;
        }
        uint8_t answer = 0;
        if (!ReadWhenReady(ports, answer)) {
            return false;
        }
        if (answer == kReplyAcknowledge) {
            return true;
        }
        if (answer != kReplyResend) {
            return false;
        }
    }
    return false;
}

}  // namespace cinux::driver::ps2
