/**
 * @file sys_probe.hpp
 * @brief Explicit unsupported Linux compatibility probes.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */
#pragma once
#include "kernel/syscall/syscall.hpp"
namespace cinux::syscall {
/**
 * @brief Reports an unsupported optional Linux facility.
 * @param[in] frame The caller's registers.
 * @return Negative ENOSYS, allowing the caller's fallback.
 * @since 0.8.0
 * @ingroup kernel_syscall
 */
long long HandleUnsupportedProbe(const SyscallFrame& frame);
}
