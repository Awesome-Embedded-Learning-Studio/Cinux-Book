/**
 * @file kernel/syscall/sys_sysinfo.hpp
 * @brief sys_sysinfo handler declaration (F-ECO batch 5)
 *
 * Fills Linux struct sysinfo for busybox `free` / `uptime` / `top`.  Backed by
 * the PMM (total/free RAM), the HPET monotonic clock (uptime), and the task
 * registry (procs count).  Fields Cinux does not track (load averages, shared
 * / buffer RAM, swap, highmem) are reported 0 -- honest, not fabricated; memunit
 * is 1 so the RAM fields are bytes (a real load-average + swap awaits the
 * accounting + swap milestones).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Linux struct sysinfo layout (x86-64, 112 bytes).  Mirrors musl/glibc so the
/// bytes we write parse identically in user space.
struct ksysinfo {
    int64_t  uptime;     ///< seconds since boot
    uint64_t loads[3];   ///< 1/5/15-min load avg (0: no load accounting yet)
    uint64_t totalram;   ///< total usable RAM, in memunit units
    uint64_t freeram;    ///< free RAM, in memunit units
    uint64_t sharedram;  ///< 0 (not tracked)
    uint64_t bufferram;  ///< 0 (not tracked)
    uint64_t totalswap;  ///< 0 (no swap)
    uint64_t freeswap;   ///< 0 (no swap)
    uint16_t procs;      ///< number of live tasks
    uint16_t pad;        ///< align the following 64-bit fields
    uint64_t totalhigh;  ///< 0 (no highmem)
    uint64_t freehigh;   ///< 0 (no highmem)
    uint32_t memunit;    ///< 1 (ram fields are bytes)
    char     unused[4];  ///< pad to 112
};
static_assert(sizeof(ksysinfo) == 112, "sysinfo is 112 bytes on x86-64");

/// sysinfo(info) -- fill the user struct sysinfo; returns 0.
int64_t sys_sysinfo(uint64_t info_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// Tests call this; sys_sysinfo is the user boundary (copy_to_user).
int64_t do_sysinfo_kernel(ksysinfo* out);

}  // namespace cinux::syscall
