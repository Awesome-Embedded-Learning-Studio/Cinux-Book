/**
 * @file kernel/arch/x86_64/msr.hpp
 * @brief Model-Specific Register helpers (F4-M3 P1-2).
 *
 * Thin inline wrappers over `wrmsr`/`rdmsr` plus the GS-base MSR indices used
 * by the per-CPU swapgs discipline.  Freestanding-safe (pure inline asm, no
 * libc).  `percpu()` reads `MSR_GS_BASE` to find this CPU's control block.
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

/// MSR_FS_BASE: per-thread TLS base (also used by tls.cpp).
constexpr uint32_t kMsrFsBase       = 0xC0000100;
/// MSR_GS_BASE: the ACTIVE GS base.  In kernel mode (after the entry swapgs)
/// it points at this CPU's PerCpu block; `percpu()` reads it.
constexpr uint32_t kMsrGsBase       = 0xC0000101;
/// MSR_KERNEL_GS_BASE: the "other" GS base swapped in by `swapgs`.  Holds the
/// user GS base while in kernel mode, and the kernel PerCpu pointer while in
/// user mode.
constexpr uint32_t kMsrKernelGsBase = 0xC0000102;

inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"(static_cast<uint32_t>(value & 0xFFFFFFFFu)),
                       "d"(static_cast<uint32_t>(value >> 32)));
}

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low  = 0;
    uint32_t high = 0;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | static_cast<uint64_t>(low);
}

}  // namespace cinux::arch
