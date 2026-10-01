/**
 * @file    console.hpp
 * @brief   The debug console every world shares: one port, one byte.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_boot
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::console {

/** @brief QEMU debugcon port; every world's PutChar answers here. */
inline constexpr uint16_t kDebugconPort = 0xE9;

/**
 * @brief         Sends one byte to the debug console.
 *
 * @param[in]     character   Byte to emit.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
inline void PutChar(char character) {
    asm volatile("outb %0, %1" : : "a"(character), "Nd"(kDebugconPort));
}

/**
 * @brief         Sends a NUL-terminated string to the debug console.
 *
 * @param[in]     text   String to emit.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
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
 * @ingroup       kernel_boot
 */
[[noreturn]] inline void Halt() {
    for (;;) {
        asm volatile("hlt");
    }
}

}  // namespace cinux::console
