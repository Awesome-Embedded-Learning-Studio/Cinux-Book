/**
 * @file sys_shm.hpp
 * @brief System V shared memory syscall handlers.
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

/** @brief Creates or finds a segment by key. @param[in] frame Linux syscall arguments. @return
 * Segment ID or negative errno. */
long long HandleShmget(const SyscallFrame& frame);
/** @brief Attaches shared pages. @param[in] frame Linux syscall arguments. @return Address or
 * negative errno. */
long long HandleShmat(const SyscallFrame& frame);
/** @brief Detaches one complete attachment. @param[in] frame Linux syscall arguments. @return Zero
 * or negative errno. */
long long HandleShmdt(const SyscallFrame& frame);
/** @brief Reads metadata or removes a segment. @param[in] frame Linux syscall arguments. @return
 * Zero or negative errno. */
long long HandleShmctl(const SyscallFrame& frame);

}  // namespace cinux::syscall
