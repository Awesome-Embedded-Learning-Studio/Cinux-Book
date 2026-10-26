/**
 * @file    sys_sendfile.hpp
 * @brief   Kernel-side transfer between two open descriptions.
 *
 * @author  Charliechen114514
 * @date    2026-10-25
 * @version 0.1
 * @since   0.8.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Transfers file bytes without a user-space data buffer.
 * @param[in]     frame rdi is out_fd, rsi in_fd, rdx an optional offset
 *                      pointer, and r10 the requested byte count.
 * @return        Bytes written, zero at EOF, or a negative errno.
 * @note          An explicit offset advances only its user word. Otherwise
 *                the input's shared cursor advances. Short writes and
 *                errors after progress preserve the completed byte count.
 * @since         0.8.0
 * @ingroup       kernel_syscall
 */
long long HandleSendfile(const SyscallFrame& frame);

}
