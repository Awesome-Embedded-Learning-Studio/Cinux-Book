/**
 * @file    console.hpp
 * @brief   The boot world console: one self-contained copy.
 *
 * The boot binary mixes 16, 32, and 64 bit code in one image, so its console
 * is header-inline and per-world free — every compiling world gets its own
 * instantiation. The kernel tree keeps its own driver-based console; this
 * copy belongs to boot alone.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_early
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::console {

/** @brief COM1 register block base. */
constexpr uint16_t kCom1Base = 0x3F8;

constexpr uint8_t kRegData = 0x0;
constexpr uint8_t kRegIer  = 0x1;
constexpr uint8_t kRegFcr  = 0x2;
constexpr uint8_t kRegLcr  = 0x3;
constexpr uint8_t kRegMcr  = 0x4;
constexpr uint8_t kRegLsr  = 0x5;

constexpr uint8_t kLsrThre = 0x20;

/**
 * @brief   One port write as data: where and what.
 * @since   0.1.0
 * @ingroup boot_early
 */
struct PortWrite {
    uint16_t port;
    uint8_t  value;
};

constexpr PortWrite kInitSequence[] = {
    {.port = kCom1Base + kRegIer, .value = 0x00},  {.port = kCom1Base + kRegLcr, .value = 0x80},
    {.port = kCom1Base + kRegData, .value = 0x01}, {.port = kCom1Base + kRegIer, .value = 0x00},
    {.port = kCom1Base + kRegLcr, .value = 0x03},  {.port = kCom1Base + kRegFcr, .value = 0xC7},
    {.port = kCom1Base + kRegMcr, .value = 0x0B}};

/**
 * @brief         Writes one byte to a port.
 *
 * @param[in]     port   Destination port.
 * @param[in]     value  Byte to emit.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_early
 */
inline void OutB(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/**
 * @brief         Reads one byte from a port.
 *
 * @param[in]     port   Source port.
 * @return        The byte the device answered.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_early
 */
inline uint8_t InB(uint16_t port) {
    uint8_t value = 0;  // NOLINT(misc-const-correctness)
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/**
 * @brief         Programs COM1 for polling output.
 *
 * @return        None
 * @note          Idempotent: 115200 8N1, FIFOs on, interrupts off.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_early
 */
inline void InitConsole() {
    for (PortWrite const& write : kInitSequence) {
        OutB(write.port, write.value);
    }
}

/**
 * @brief         Sends one character, waiting for the line to be ready.
 *
 * @param[in]     character   Byte to transmit.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_early
 */
inline void PutChar(char character) {
    while ((InB(kCom1Base + kRegLsr) & kLsrThre) == 0) {
        asm volatile("pause");
    }
    OutB(kCom1Base + kRegData, static_cast<uint8_t>(character));
}

/**
 * @brief         Sends a NUL-terminated string.
 *
 * @param[in]     text   String to emit.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_early
 */
inline void PutString(const char* text) {
    while (*text != '\0') {
        PutChar(*text);
        ++text;
    }
}

/**
 * @brief         Parks the current world forever.
 *
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_early
 */
[[noreturn]] inline void Halt() {
    for (;;) {
        asm volatile("hlt");
    }
}

}  // namespace cinux::console
