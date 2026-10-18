/**
 * @file    sys_ppoll.hpp
 * @brief   Declaration of the ppoll(fds, nfds, timeout, mask) syscall handler.
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
 * @brief         ppoll(ufds, nfds, timeout, sigmask): wait until some
 *                descriptor is ready, the timeout runs out, or a
 *                signal wakes the caller.
 *
 * @return        How many entries carry a nonzero answer, zero at
 *                timeout, or a negative errno.
 * @note          The served answers: POLLIN from the terminal's line
 *                pool (descriptors 0 through 2) and from pipe bytes,
 *                POLLOUT from the console pair and from pipe room,
 *                POLLHUP when a pipe's last writer said farewell,
 *                POLLNVAL for a descriptor the file table does not
 *                serve. Entries naming a negative descriptor get a
 *                zero answer and are not counted. The timeout waits
 *                through cooperative yields against a monotonic
 *                deadline; an unblocked pending signal returns
 *                -EINTR so the caller may reap the news. The signal
 *                mask argument rides along unexamined — masking for
 *                the wait needs a real sleep-wake and belongs to a
 *                later station.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long HandlePpoll(const SyscallFrame& frame);

}  // namespace cinux::syscall
