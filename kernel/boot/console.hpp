/**
 * @file    console.hpp
 * @brief   The console the kernel world speaks through.
 *
 * A thin face over the serial driver: characters and strings out.
 Formatting lives one layer up in print.hpp; ports stay two layers
 down — nobody skips a floor. Parking the CPU is not a console concern;
 Halt lives in arch (kernel/arch/x86_64/halt.hpp).
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
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

}  // namespace cinux::console
