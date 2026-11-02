/** @file sys_capability.hpp
 * @brief Capability ABI for the kernel's empty capability sets.
 */
#pragma once
#include "kernel/syscall/syscall.hpp"
namespace cinux::syscall {
long long HandleCapget(const SyscallFrame& frame);
long long HandleCapset(const SyscallFrame& frame);
}
