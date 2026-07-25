/**
 * @file kernel/syscall/sys_prlimit64.hpp
 * @brief sys_prlimit64 (Linux 302) handler declaration (gcc/g++ self-host batch)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_rlim_virt,
                      uint64_t old_rlim_virt, uint64_t, uint64_t);

}  // namespace cinux::syscall
