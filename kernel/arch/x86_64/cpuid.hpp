/**
 * @file kernel/arch/x86_64/cpuid.hpp
 * @brief CPUID helpers (F4-B0)
 *
 * Used so far by AT_HWCAP emission (user_launch.cpp) so glibc's IFUNC resolvers
 * can pick SIMD-optimized memcpy/strlen/... paths instead of falling back to the
 * generic C loops. Linux puts CPUID.01H:EDX (FPU/SSE/SSE2/...) into AT_HWCAP;
 * the extended ECX bits and CPUID.07H leaves go to AT_HWCAP2, which Cinux does
 * not yet emit (follow-up if a toolchain binary needs AVX-class dispatch).
 *
 * Freestanding-friendly: no <bitset>, just inline asm + uint32_t outs.
 */

#pragma once

#include <cstdint>

namespace cinux::arch {

/// Run CPUID with @p leaf in EAX (sub-leaf 0). Writes the four register outs.
inline void cpuid(uint32_t leaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(0));
}

/// AT_HWCAP value: CPUID.01H feature flags. Returns EDX zero-extended to 64 bits
/// (Linux's classic AT_HWCAP encoding: FPU=bit0, ... SSE=bit25, SSE2=bit26, HT).
/// glibc reads these off AT_HWCAP at startup; 0 (the old value) disables every
/// SIMD path and forces the slow generic implementations.
inline uint64_t hwcap_from_cpuid() {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(1, eax, ebx, ecx, edx);
    return static_cast<uint64_t>(edx);
}

}  // namespace cinux::arch
