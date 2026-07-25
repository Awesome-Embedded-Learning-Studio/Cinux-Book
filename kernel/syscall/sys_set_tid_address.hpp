/**
 * @file kernel/syscall/sys_set_tid_address.hpp
 * @brief sys_set_tid_address handler declaration (F10-M1 batch 4)
 *
 * Records the tid-clearing address used by the pthread_join / thread-exit
 * protocol, and returns the caller's tid.  musl's __init_tp() calls it
 * unconditionally during static startup and stores the return value as the
 * thread's tid, so an unimplemented/zero return corrupts the TCB.
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// set_tid_address(tidptr) -- record cleartid addr, return caller tid.
int64_t sys_set_tid_address(uint64_t tidptr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
