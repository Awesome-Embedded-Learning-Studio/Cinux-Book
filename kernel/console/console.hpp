/**
 * @file    console.hpp
 * @brief   The console the kernel world speaks through.
 *
 * A neutral face: characters and strings out, nothing about devices.
 * Formatting lives one layer up in print.hpp; the sinks — the serial
 * line first, the text screen when its facts hold — live below and are
 * wired by this face's implementation, so no caller ever names one.
 * Parking the CPU is not a console concern; Halt lives in arch
 * (kernel/arch/x86_64/halt.hpp).
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.3
 * @since   0.1.0
 * @ingroup kernel_boot
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/boot/boot_info.hpp"

namespace cinux::console {

/**
 * @brief         Brings the kernel console up: serial first so debugging
 *                never waits on a screen, then the text screen when the
 *                boot record carries a usable framebuffer.
 *
 * @param[in]     info   The boot handoff record.
 * @return        None
 * @note          Safe to call again; a screen that refused init stays
 *                silent and serial keeps carrying the whole output.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
/**
 * @brief         Brings the kernel console up: serial first so debugging
 *                never waits on a screen, then the text screen when the
 *                boot record carries a usable framebuffer.
 *
 * @param[in]     info   The boot handoff record.
 * @return        None
 * @note          Safe to call again; a screen that refused init stays
 *                silent and serial keeps carrying the whole output.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
void InitConsole(const cinux::boot::BootInfo& info);

/**
 * @brief         Sends one character to every console sink.
 *
 * @param[in]     character   Byte to emit.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
void PutChar(char character);

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
