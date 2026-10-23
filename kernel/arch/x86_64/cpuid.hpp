/**
 * @file    cpuid.hpp
 * @brief   The named facts the processor answers about itself.
 *
 * CPUID is the processor's self-description service: one instruction,
 * a leaf number in EAX (and a subleaf in ECX for the strided leaves),
 * four words of answer. The kernel asks it exactly one kind of
 * question — "is this CR4 bit implemented?" — and every protection bit
 * from the 2010s answers through leaf 7. Header inline on purpose: the
 * question carries no state and each world asks it for free.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch {

/// The four words CPUID fills for one (leaf, subleaf) question.
struct CpuIdWords {
    /// EAX on return; for leaf 0 it names the highest standard leaf.
    unsigned int eax{};

    /// EBX on return; leaf 7 carries the protection-feature bitmap here.
    unsigned int ebx{};

    /// ECX on return.
    unsigned int ecx{};

    /// EDX on return.
    unsigned int edx{};
};

/**
 * @brief         Asks the processor one CPUID question.
 *
 * @param[in]     leaf     The main selector, EAX on entry.
 * @param[in]     subleaf  The strided selector, ECX on entry; leaves
 *                         without strides ignore it and pass zero.
 * @return        The four answer words.
 * @note          Asking a leaf beyond the processor's knowledge is
 *                undefined territory — the named facts below ask leaf 0
 *                first and so should any new question.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline CpuIdWords ReadCpuId(unsigned int leaf, unsigned int subleaf) {
    CpuIdWords   answer{};
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned int selected = leaf;
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned int stride   = subleaf;
    asm volatile("cpuid" : "+a"(selected), "+b"(answer.ebx), "+c"(stride), "+d"(answer.edx));
    answer.eax = selected;
    static_cast<void>(stride);
    return answer;
}

/**
 * @brief         Reports whether the processor implements SMEP.
 *
 * @return        True when CPUID leaf 7 names CR4 bit 20 available.
 * @note          Writing the bit into CR4 on a processor without it
 *                fails the whole write with #GP, so the enable step
 *                asks here first and skips cleanly when the answer is
 *                no.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline bool HasSmep() {
    if (ReadCpuId(0U, 0U).eax < 7U) {
        return false;
    }
    return (ReadCpuId(7U, 0U).ebx & (1U << 7U)) != 0U;
}

/**
 * @brief         Reports whether the processor implements SMAP.
 *
 * @return        True when CPUID leaf 7 names CR4 bit 21 available.
 * @note          Same discipline as SMEP: ask first, write only on a
 *                yes, and the mechanism printout tells the truth when
 *                the answer is no.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline bool HasSmap() {
    if (ReadCpuId(0U, 0U).eax < 7U) {
        return false;
    }
    return (ReadCpuId(7U, 0U).ebx & (1U << 20U)) != 0U;
}

}  // namespace cinux::arch
