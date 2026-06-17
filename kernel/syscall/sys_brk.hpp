/**
 * @file kernel/syscall/sys_brk.hpp
 * @brief sys_brk handler declaration (F2-M3)
 *
 * Adjusts the program break (user heap end).  Lazy: only moves brk_current --
 * the Heap VMA (created by execve) covers the whole window and pages are
 * demand-paged on first access.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Set the program break (Linux syscall 12)
 *
 * @param addr  New break address, or 0 to query.
 * @return The new break on success, the current break if @p addr is out of
 *         range (< brk_initial or > brk_max).  Never returns an error.
 */
int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
