/**
 * @file sys_epoll.hpp
 * @brief Level-triggered epoll readiness over open descriptions.
 */
#pragma once
#include "kernel/syscall/syscall.hpp"
namespace cinux::syscall {
/**
 * @brief Create a close-on-exec-capable epoll descriptor.
 * @return The result or a negative Linux errno.
 */
long long HandleEpollCreate1(const SyscallFrame& frame);
/**
 * @brief Add, change, or remove an open-description registration.
 * @return The result or a negative Linux errno.
 */
long long HandleEpollCtl(const SyscallFrame& frame);
/**
 * @brief Wait for readiness, timeout, or a pending signal.
 * @return The result or a negative Linux errno.
 */
long long HandleEpollWait(const SyscallFrame& frame);
/**
 * @brief Wait with a temporary signal mask.
 * @return The result or a negative Linux errno.
 */
long long HandleEpollPwait(const SyscallFrame& frame);
}
