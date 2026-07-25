/**
 * @file kernel/syscall/sys_clone.cpp
 * @brief sys_clone handler implementation (F3-M2 batch 4)
 *
 * Thin wrapper that forwards to cinux::proc::clone().
 */

#include "kernel/syscall/sys_clone.hpp"

#include "kernel/proc/process.hpp"

namespace cinux::syscall {

int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t child_tid,
                  uint64_t tls, uint64_t) {
    return static_cast<int64_t>(cinux::proc::clone(flags, stack, parent_tid, child_tid, tls));
}

}  // namespace cinux::syscall
