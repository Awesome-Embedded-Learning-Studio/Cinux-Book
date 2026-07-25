/**
 * @file kernel/syscall/sys_getrandom.hpp
 * @brief sys_getrandom (Linux 318) handler declaration (gcc/g++ self-host batch)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

int64_t sys_getrandom(uint64_t buf_virt, uint64_t len, uint64_t flags, uint64_t, uint64_t,
                      uint64_t);

}  // namespace cinux::syscall
