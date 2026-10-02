#include "kernel/driver/serial.hpp"

#include <stdint.h>

#include "kernel/driver/base/io.hpp"

namespace cinux::driver {

namespace {

constexpr uint16_t kCom1Base = 0x3F8;

constexpr uint8_t kRegData = 0x0;
constexpr uint8_t kRegIer  = 0x1;
constexpr uint8_t kRegFcr  = 0x2;
constexpr uint8_t kRegLcr  = 0x3;
constexpr uint8_t kRegMcr  = 0x4;
constexpr uint8_t kRegLsr  = 0x5;

constexpr uint8_t kLsrThre = 0x20;

constexpr PortWrite kInitSequence[] = {
    {.port = kCom1Base + kRegIer, .value = 0x00},  {.port = kCom1Base + kRegLcr, .value = 0x80},
    {.port = kCom1Base + kRegData, .value = 0x01}, {.port = kCom1Base + kRegIer, .value = 0x00},
    {.port = kCom1Base + kRegLcr, .value = 0x03},  {.port = kCom1Base + kRegFcr, .value = 0xC7},
    {.port = kCom1Base + kRegMcr, .value = 0x0B}};

}  // namespace

void SerialInit() {
    OutB(kInitSequence);
}

void SerialPutChar(char character) {
    WaitBitsSet(kCom1Base + kRegLsr, kLsrThre);
    OutB({.port = kCom1Base + kRegData, .value = static_cast<uint8_t>(character)});
}

}  // namespace cinux::driver
