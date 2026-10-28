/**
 * @file    controller.hpp
 * @brief   The 8042 PS/2 controller's shared transaction vocabulary.
 *
 * The keyboard and the auxiliary pointer share one controller, so the
 * port pair, the command words, and the answer discipline live here
 * once. Transactions travel through an explicit port seam: the kernel
 * wires the seam to real port I/O, host tests wire it to a scripted
 * answer stream. Every wait uses the supplied IRQ-independent delay, so an
 * absent device ends its bring-up in finite time.
 *
 * @author  Charliechen114514
 * @date    2026-10-28
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/time.hpp"

namespace cinux::driver::ps2 {

/// One controller's port operations, supplied by the caller.
struct Ps2Ports {
    void* context                                               = nullptr;
    /// Reads the status register.
    uint8_t (*status)(void* context)                            = nullptr;
    /// Reads one pending data byte.
    uint8_t (*data_in)(void* context)                           = nullptr;
    /// Writes one command byte.
    void (*command_out)(void* context, uint8_t value)           = nullptr;
    /// Writes one data byte.
    void (*data_out)(void* context, uint8_t value)              = nullptr;
    /// Delay against an IRQ-independent timer, at least the requested duration.
    void (*delay)(void* context, base::time::Microseconds span) = nullptr;
};

/// The real controller's seam, wired over port I/O.
[[nodiscard]] Ps2Ports HostPs2Ports();

/// The data port every transaction's bytes cross.
inline constexpr uint16_t kDataPort = 0x60;

/// The configuration byte's first-port IRQ gate.
inline constexpr uint8_t kConfigIrqFirst = 0x01;

/// The configuration byte's second-port IRQ gate.
inline constexpr uint8_t kConfigIrqSecond = 0x02;

/// Disables the first port, the keyboard's channel.
inline constexpr uint8_t kCommandDisableFirst = 0xAD;

/// Disables the second port, the auxiliary channel.
inline constexpr uint8_t kCommandDisableSecond = 0xA7;

/// Enables the first port again.
inline constexpr uint8_t kCommandEnableFirst = 0xAE;

/// Enables the second port again.
inline constexpr uint8_t kCommandEnableSecond = 0xA8;

/// Asks the controller to hand back its configuration byte.
inline constexpr uint8_t kCommandReadConfig = 0x20;

/// Sends the next data byte as the configuration byte.
inline constexpr uint8_t kCommandWriteConfig = 0x60;

/// Runs the controller's self test; the answer byte says healthy.
inline constexpr uint8_t kCommandSelfTest = 0xAA;

/// The self test's healthy answer.
inline constexpr uint8_t kSelfTestHealthy = 0x55;

/// Routes the next data byte to the auxiliary device.
inline constexpr uint8_t kCommandWriteAux = 0xD4;

/**
 * @brief         Writes one command byte once the input buffer drains.
 * @param[in]     ports    The controller seam.
 * @param[in]     command  The command word.
 * @return        True on completion; false when the controller stays busy.
 * @since         0.9.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool SendCommand(Ps2Ports ports, uint8_t command);

/**
 * @brief         Writes one data byte once the input buffer drains.
 * @param[in]     ports  The controller seam.
 * @param[in]     value  The data byte.
 * @return        True on completion; false when the controller stays busy.
 * @since         0.9.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool WriteData(Ps2Ports ports, uint8_t value);

/// Waits for the output buffer to fill, then reads the byte it carries.
[[nodiscard]] bool ReadWhenReady(Ps2Ports ports, uint8_t& out);

/**
 * @brief         Drops every byte already waiting in the output buffer.
 * @param[in]     ports   The controller seam.
 * @return        True on completion; false when the controller stays busy.
 * @since         0.9.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool Drain(Ps2Ports ports);

/**
 * @brief         One host-to-auxiliary transaction under the device's
 *                answer discipline.
 *
 * @param[in]     ports   The controller seam.
 * @param[in]     value   The byte the auxiliary device receives.
 * @return        True only when the device acknowledged; a resend
 *                request is honoured within a bounded budget and an
 *                error byte or a silent device fails the transaction.
 * @note          Acknowledges arrive within milliseconds of the spec's
 *                timing; the budget mirrors that scale instead of an
 *                arbitrary spin count.
 * @since         0.9.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool SendAux(Ps2Ports ports, uint8_t value);

}  // namespace cinux::driver::ps2
