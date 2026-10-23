/**
 * @file    instructions.hpp
 * @brief   Little execution-hint primitives the kernel spells as asm.
 *
 * One-line instructions that mean something to the CPU but nothing to
 * C++ gather here under plain names, so call sites read intent instead
 * of AT&T syntax. Today: the spin-wait hint and the model-specific
 * register pair. The interrupt save and restore pair lives one file
 * over in irq_guard.hpp; heavier machine work (context switch) stays
 * in its own assembly file.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch {

/**
 * @brief         The spin-wait hint: tell the CPU this loop is waiting.
 * @return        None.
 * @note          Pauses the pipeline a beat, saves power, and lets a
 *                hyperthread sibling make progress — the polite thing
 *                to do inside a spin loop. No memory semantics.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
inline void CpuRelax() {
    __asm__ volatile("pause" : : : "memory");
}

/**
 * @brief         Read one model-specific register.
 * @param[in]     index  The MSR index the CPU expects in ECX.
 * @return        The full 64-bit value, assembled from EDX:EAX.
 * @note          Reading a reserved or unsupported index raises #GP,
 *                so callers name the index they mean and own that
 *                contract. The QEMU caveat: not every MSR survives a
 *                write-read round trip even when the write is accepted.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
inline unsigned long long ReadMsr(unsigned int index) {
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned int low  = 0;
    // NOLINTNEXTLINE(misc-const-correctness)
    unsigned int high = 0;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return (static_cast<unsigned long long>(high) << 32U) | static_cast<unsigned long long>(low);
}

/**
 * @brief         Write one model-specific register.
 * @param[in]     index  The MSR index the CPU expects in ECX.
 * @param[in]     value  The full 64-bit value, split into EDX:EAX.
 * @return        None.
 * @note          Writing a reserved or unsupported value raises #GP.
 *                The instruction only consumes the low 32 bits of each
 *                half, so the split here is the whole story — shifting
 *                a 64-bit register and hoping the top survives was the
 *                classic first-jump bug.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
inline void WriteMsr(unsigned int index, unsigned long long value) {
    auto const kLow  = static_cast<unsigned int>(value & 0xFFFFFFFFULL);
    auto const kHigh = static_cast<unsigned int>(value >> 32U);
    __asm__ volatile("wrmsr" : : "c"(index), "a"(kLow), "d"(kHigh) : "memory");
}

/**
 * @brief         Opens the SMAP window for the current instruction.
 *
 * @return        None.
 * @note          Sets RFLAGS.AC, the one bit that lets ring 0 touch user
 *                pages while CR4.SMAP is on. The window must close before
 *                anything blocks or schedules — AC belongs to the CPU that
 *                opened it, and a context switch would drop it mid-copy.
 *                Executing the instruction on a processor without SMAP in
 *                its CPUID answer faults with #UD, so the accessor keeps
 *                the arm bit and only reaches for the window on machines
 *                that promised one.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline void Stac() {
    __asm__ volatile("stac" : : : "memory");
}

/**
 * @brief         Closes the SMAP window the current instruction opened.
 *
 * @return        None.
 * @note          Clears RFLAGS.AC. Every open has exactly one close on
 *                every path out of the window, including the fault path,
 *                where the fixup closes it before reporting the copy
 *                failed.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline void Clac() {
    __asm__ volatile("clac" : : : "memory");
}

}  // namespace cinux::arch
