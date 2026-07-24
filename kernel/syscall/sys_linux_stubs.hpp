/**
 * @file kernel/syscall/sys_linux_stubs.hpp
 * @brief Linux ABI probing stubs (gcc/g++ self-host syscall batch, 2026-07-05)
 *
 * glibc/musl probe several Linux syscalls at startup or link time to detect
 * kernel features.  Cinux does not implement the feature behind them; the
 * probe only needs a value the libc can fall back from.  Registering these
 * (vs leaving them unhandled) also stops the dispatch log from spamming
 * "unhandled syscall N" on every compile.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

int64_t sys_getcpu(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_rseq(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_clone3(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_set_robust_list(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_sendfile(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t,
                               uint64_t, uint64_t);
int64_t sys_setitimer(uint64_t which, uint64_t new_value, uint64_t old_value, uint64_t, uint64_t,
                      uint64_t);
int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
