/**
 * @file kernel/syscall/sys_getrusage.hpp
 * @brief sys_getrusage handler declaration (F-ECO batch 5)
 *
 * Fills Linux struct rusage.  CinuxOS does not yet do per-task resource
 * accounting (no user/system CPU time, no fault / context-switch counters, no
 * max-RSS), so every field is reported 0 -- honest, not fabricated.  busybox
 * `time` / `top` link but read zeros until an accounting milestone wires real
 * values.  @p who is validated against the RUSAGE_* set (SELF / CHILDREN /
 * THREAD, covering the kernel + libc numeric conventions); unknown -> -EINVAL.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Linux struct timeval layout (x86-64): { time_t tv_sec; suseconds_t tv_usec; }.
struct ktimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/// Linux struct rusage layout (x86-64, 144 bytes).  Mirrors musl/glibc.
struct krusage {
    ktimeval ru_utime;     ///< user CPU time (0: not tracked)
    ktimeval ru_stime;     ///< system CPU time (0)
    int64_t  ru_maxrss;    ///< maximum resident set size (KB) (0)
    int64_t  ru_ixrss;     ///< integral shared memory (0)
    int64_t  ru_idrss;     ///< integral unshared data (0)
    int64_t  ru_isrss;     ///< integral unshared stack (0)
    int64_t  ru_minflt;    ///< minor page faults (0)
    int64_t  ru_majflt;    ///< major page faults (0)
    int64_t  ru_nswap;     ///< swaps (0)
    int64_t  ru_inblock;   ///< block input ops (0)
    int64_t  ru_oublock;   ///< block output ops (0)
    int64_t  ru_msgsnd;    ///< messages sent (0)
    int64_t  ru_msgrcv;    ///< messages received (0)
    int64_t  ru_nsignals;  ///< signals received (0)
    int64_t  ru_nvcsw;     ///< voluntary context switches (0)
    int64_t  ru_nivcsw;    ///< involuntary context switches (0)
};
static_assert(sizeof(krusage) == 144, "rusage is 144 bytes on x86-64");

/// getrusage(who, usage) -- fill rusage (zeros until accounting exists); 0 / -EINVAL.
int64_t sys_getrusage(uint64_t who, uint64_t usage_virt, uint64_t, uint64_t, uint64_t, uint64_t);

/// Tests call this; sys_getrusage is the user boundary (copy_to_user).
int64_t do_getrusage_kernel(uint64_t who, krusage* out);

}  // namespace cinux::syscall
