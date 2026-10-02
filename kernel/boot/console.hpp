/**
 * @file    console.hpp
 * @brief   The console the kernel world speaks through.
 *
 * A thin face over the serial driver: characters and strings out, and a
 clean park at the end. Formatting lives one layer up in print.hpp;
 ports stay two layers down — nobody skips a floor.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_boot
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/driver/serial.hpp"

namespace cinux::console {

/**
 * @brief         Brings the kernel console up.
 *
 * @return        None
 * @note          Calls the serial driver init; safe to call again.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
inline void InitConsole() {
    cinux::driver::SerialInit();
}

/**
 * @brief         Sends one character to the console.
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
    cinux::driver::SerialPutChar(character);
}

/**
 * @brief         Sends a NUL-terminated string, one character at a time.
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
 * @brief         Parks the CPU forever with interrupts off the path.
 *
 * @return        None
 * @note          The final stop of every early failure and every clean end.
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
