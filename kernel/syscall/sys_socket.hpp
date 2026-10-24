/**
 * @file    sys_socket.hpp
 * @brief   Declarations of the socket-family syscall handlers.
 *
 * The descriptor-numbered door into the socket family. Every handler
 * finds the descriptor's socket through the shared erased face, moves
 * addresses through the accessor with the wire's own byte order, and
 * answers the errno words a libc maps without asking. The blocking
 * manners of receive live here, not in the sockets: would-block turns
 * into a wait, and a deliverable signal ends it with EINTR.
 *
 * @author  Charliechen114514
 * @date    2026-10-24
 * @version 0.1
 * @since   0.7.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         socket(domain, type, protocol): a new end, one file.
 * @return        The descriptor number, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleSocket(const SyscallFrame& frame);

/**
 * @brief         bind(fd, addr, len): seat the socket on an address.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleBind(const SyscallFrame& frame);

/**
 * @brief         connect(fd, addr, len): name the far end.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleConnect(const SyscallFrame& frame);

/**
 * @brief         sendto(fd, buf, len, flags, addr, len): one datagram out.
 * @return        The bytes given, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleSendto(const SyscallFrame& frame);

/**
 * @brief         recvfrom(fd, buf, len, flags, addr, len): one datagram in.
 * @return        The bytes taken, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleRecvfrom(const SyscallFrame& frame);

/**
 * @brief         shutdown(fd, how): end one or both directions.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleShutdown(const SyscallFrame& frame);

/**
 * @brief         listen(fd, backlog): offer to answer knocks.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleListen(const SyscallFrame& frame);

/**
 * @brief         accept(fd, addr, len): answer one knocking caller.
 * @return        The new descriptor, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleAccept(const SyscallFrame& frame);

/**
 * @brief         accept4(fd, addr, len, flags): accept with descriptor flags.
 * @return        The new descriptor, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleAccept4(const SyscallFrame& frame);

/**
 * @brief         getsockname(fd, addr, len): the seated address.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleGetsockname(const SyscallFrame& frame);

/**
 * @brief         getpeername(fd, addr, len): the named far end.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleGetpeername(const SyscallFrame& frame);

/**
 * @brief         socketpair(domain, type, protocol, fds): two ends at once.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleSocketpair(const SyscallFrame& frame);

/**
 * @brief         setsockopt(fd, level, name, val, len): turn one knob.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleSetsockopt(const SyscallFrame& frame);

/**
 * @brief         getsockopt(fd, level, name, val, len): read one knob.
 * @return        Zero, or a negative errno.
 * @since         0.7.0
 * @ingroup       kernel_syscall
 */
long long HandleGetsockopt(const SyscallFrame& frame);

}  // namespace cinux::syscall
