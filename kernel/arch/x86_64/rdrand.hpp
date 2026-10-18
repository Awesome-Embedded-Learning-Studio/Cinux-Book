/**
 * @file    rdrand.hpp
 * @brief   The CPU random-number instruction, probed and drawn.
 *
 * RDRAND puts a random 64-bit value in a register and reports
 * success in the carry flag. The probe is CPUID.01H:ECX bit 30.
 * Both are single instructions, so the file stays header-only:
 * every world gets a free instantiation, and a .cpp would only
 * add a compile unit for the same two asm blocks.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

/**
 * @brief         Reports whether the CPU advertises RDRAND.
 * @return        True when CPUID.01H:ECX bit 30 is set.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline bool HasRdrand() {
    constexpr unsigned int kRdrandFeatureBit = 1U << 30U;
    unsigned int           eax = 1U;  // NOLINT(misc-const-correctness) written by the asm output
    unsigned int           ebx = 0U;  // NOLINT(misc-const-correctness) written by the asm output
    unsigned int           ecx = 0U;  // NOLINT(misc-const-correctness) written by the asm output
    unsigned int           edx = 0U;  // NOLINT(misc-const-correctness) written by the asm output
    asm volatile("cpuid" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx));
    return (ecx & kRdrandFeatureBit) != 0U;
}

/**
 * @brief         Draws one 64-bit hardware random word.
 * @param[out]    out   Receives the word on success.
 * @return        True when the carry flag reported a valid draw.
 * @note          The instruction sets CF on success; the x86 manual
 *                allows transient failures and suggests a bounded
 *                retry loop, so the draw retries a handful of times
 *                before reporting failure upward.
 * @since         0.3.0
 * @ingroup       kernel_arch
 * @copyright Copyright (c) 2026
 */
inline bool TryRdrand(unsigned long long& out) {
    for (unsigned int attempt = 0; attempt < 10; ++attempt) {
        unsigned long long value = 0;  // NOLINT(misc-const-correctness) written by the asm output
        unsigned char      carry = 0;  // NOLINT(misc-const-correctness) written by the asm output
        asm volatile(
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(value), "=qm"(carry)
            :
            : "cc");
        if (carry != 0) {
            out = value;
            return true;
        }
    }
    return false;
}

}  // namespace cinux::arch
