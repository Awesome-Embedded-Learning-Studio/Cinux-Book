/**
 * @file kernel/syscall/sys_time.hpp
 * @brief sys_gettimeofday (Linux 96) and sys_time (Linux 201) declarations
 *        (gcc/g++ self-host batch, 2026-07-05)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

int64_t sys_gettimeofday(uint64_t tv_virt, uint64_t tz_virt, uint64_t, uint64_t, uint64_t,
                         uint64_t);
int64_t sys_time(uint64_t t_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
