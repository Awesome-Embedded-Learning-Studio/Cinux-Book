/**
 * @file kernel/syscall/sys_clone.hpp
 * @brief clone syscall handler (F3-M2 batch 4)
 *
 * Linux syscall 56: create a new task sharing resources per the CLONE_*
 * flags.  Delegates to cinux::proc::clone().
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief clone(flags, stack, parent_tid, child_tid, tls)
 *
 * @return child tid (>0) to the parent, or -errno on failure.
 */
int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t child_tid,
                  uint64_t tls, uint64_t);

}  // namespace cinux::syscall
