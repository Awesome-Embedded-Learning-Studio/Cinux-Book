/**
 * @file    sys_ioctl.hpp
 * @brief   Declaration of the ioctl(fd, cmd, arg) syscall handler.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Device questions: one served answer, everything else
 *                not-a-terminal.
 *
 * @return        Zero on the served ask, or a negative errno.
 * @note          TCGETS is how the hosted world asks "is this a
 *                terminal": the console pair answers with an all-zero
 *                termios record so isatty turns true and job control
 *                may arm. A real tty with line discipline is a later
 *                station's work; file descriptors answer ENOTTY, the
 *                hosted word for "no terminal here".
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandleIoctl(const SyscallFrame& frame);

}  // namespace cinux::syscall
