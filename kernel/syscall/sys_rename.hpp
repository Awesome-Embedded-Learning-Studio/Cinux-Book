/** @file sys_rename.hpp
 * @brief Persistent file-name and permission changes.
 */
#pragma once
#include "kernel/syscall/syscall.hpp"
namespace cinux::syscall {
/**
 * @brief Move a file between paths in the same mounted backend.
 * @return The result or a negative Linux errno.
 */
long long HandleRename(const SyscallFrame& frame);
/**
 * @brief Set permission bits for a named inode.
 * @return The result or a negative Linux errno.
 */
long long HandleChmod(const SyscallFrame& frame);
}
