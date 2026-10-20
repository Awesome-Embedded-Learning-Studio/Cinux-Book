/**
 * @file    console_tty.hpp
 * @brief   The one terminal wired to the console pair.
 *
 * The discipline class itself owns no plumbing; this is the kernel's one
 * assembled instance: serial-then-keyboard for input, the console face
 * for echo, the scheduler's yield for waiting, and the text screen's own
 * shape for the window ioctl. fd zero's readers and /dev/console's
 * openers reach the same terminal through here.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/driver/tty/tty.hpp"

namespace cinux::driver {

/**
 * @brief         The console terminal, wired and ready.
 *
 * @return        The one Tty on the serial+keyboard seats.
 * @note          Lazily assembled at first use, by which time the boot
 *                banner has already shaped the screen; it lives for the
 *                rest of the boot.
 * @since         0.5.0
 * @ingroup       kernel_driver
 */
Tty& ConsoleTty();

}  // namespace cinux::driver
