/**
 * @file    context.hpp
 * @brief   The register image a task switch saves and restores.
 *
 * CpuContext is the C++ mirror of context_switch.S: eight callee-saved
 * slots the assembly moves with plain offsets, then one 512-byte
 * FXSAVE64 image so a task's x87/SSE state travels with it. Every
 * offset the assembly hard-codes is asserted right here, so the two
 * files can only agree or fail the build — the struct changes, an
 * assert breaks, nobody computes offsets by hand. The general-purpose
 * half is deliberately small: a switch happens at known call
 * boundaries where caller-saved registers are already dead, which is
 * exactly what the callee-saved convention guarantees.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>

namespace cinux::arch {

/**
 * @brief         Aligned x87, MXCSR and SSE register image for FXSAVE64.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
struct alignas(16) FpuImage {
    unsigned long long words[64];
};

static_assert(sizeof(FpuImage) == 512, "FXSAVE64 writes 512 bytes");
static_assert(alignof(FpuImage) >= 16, "FXSAVE64 demands 16-byte alignment");

/**
 * @brief         Save the live floating-point state into an aligned image.
 * @param[out]    image   Receives the x87, MXCSR and SSE registers.
 * @return        None.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
// NOLINTNEXTLINE(misc-const-correctness)
inline void SaveFpu(FpuImage& image) {
    asm volatile("fxsave64 %0" : "=m"(image) : : "memory");
}

/**
 * @brief         Restore floating-point state from an aligned image.
 * @param[in]     image   A valid FXSAVE64 register image.
 * @return        None.
 * @note          Call from code compiled with general registers only.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
inline void RestoreFpu(const FpuImage& image) {
    asm volatile("fxrstor64 %0" : : "m"(image) : "memory");
}

/**
 * @brief         Callee-saved register snapshot plus FXSAVE64 image.
 * @note          Field order and offsets are load-bearing: they are
 *                the contract with context_switch.S, guarded by the
 *                static_asserts below. fpu is 16-byte aligned so the
 *                FXSAVE64 the switcher issues is legal on every task.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
struct alignas(16) CpuContext {
    unsigned long long r15;  ///< Slot 0, offset 0.
    unsigned long long r14;  ///< Slot 1, offset 8.
    unsigned long long r13;  ///< Slot 2, offset 16.
    unsigned long long r12;  ///< Slot 3, offset 24.
    unsigned long long rbp;  ///< Slot 4, offset 32.
    unsigned long long rbx;  ///< Slot 5, offset 40.
    unsigned long long rsp;  ///< Slot 6, offset 48.
    unsigned long long rip;  ///< Slot 7, offset 56.
    FpuImage           fpu;
    /// FS base (MSR 0xC0000100), the thread pointer; moved by the C++
    /// side around the switch, not by the assembly.
    unsigned long long fs_base;
    /// GS base as the task owns it. While any ring runs, the live
    /// IA32_GS_BASE stays pointed at the per-CPU block; this value
    /// travels through IA32_KERNEL_GSBASE and is re-staged on every
    /// switch.
    unsigned long long gs_base;
};

static_assert(offsetof(CpuContext, r15) == 0, "assembly reads r15 at 0");
static_assert(offsetof(CpuContext, r14) == 8, "assembly reads r14 at 8");
static_assert(offsetof(CpuContext, r13) == 16, "assembly reads r13 at 16");
static_assert(offsetof(CpuContext, r12) == 24, "assembly reads r12 at 24");
static_assert(offsetof(CpuContext, rbp) == 32, "assembly reads rbp at 32");
static_assert(offsetof(CpuContext, rbx) == 40, "assembly reads rbx at 40");
static_assert(offsetof(CpuContext, rsp) == 48, "assembly reads rsp at 48");
static_assert(offsetof(CpuContext, rip) == 56, "assembly reads rip at 56");
static_assert(offsetof(CpuContext, fpu) == 64, "fxsave64 image lives at 64");
static_assert(offsetof(CpuContext, fs_base) == 576, "fs_base rides behind the image");
static_assert(offsetof(CpuContext, gs_base) == 584, "gs_base closes the context");
static_assert(sizeof(CpuContext) == 592, "8 slots, one 512-byte image, two base words");
static_assert(alignof(CpuContext) >= 16, "FXSAVE64 demands 16-byte alignment");

/// Highest canonical address a ring-3 world may occupy.
inline constexpr unsigned long long kCanonicalUserTop = 0x00007FFFFFFFFFFFULL;

/// Lowest canonical address in the kernel half.
inline constexpr unsigned long long kCanonicalKernelBottom = 0xFFFF800000000000ULL;

/**
 * @brief         Whether an address is canonical on x86-64.
 * @param[in]     value   The address to judge.
 * @return        true when bits 63..47 all agree, the shape every
 *                base MSR demands — a non-canonical value handed to
 *                wrmsr raises #GP instead of returning an error.
 * @since         0.8.0
 * @ingroup       kernel_arch
 */
[[nodiscard]] constexpr bool IsCanonicalAddress(unsigned long long value) {
    return value <= kCanonicalUserTop || value >= kCanonicalKernelBottom;
}

}  // namespace cinux::arch

extern "C" {

/**
 * @brief         Save the outgoing task, restore the incoming one, and
 *                never return normally.
 * @param[in,out] outgoing   Context that receives the current registers.
 * @param[in]     incoming   Context whose registers become live.
 * @return        None.
 * @note          Control continues at incoming.rip. When the outgoing
 *                task is switched back in, execution lands at the
 *                internal resume point and this call returns as if it
 *                never left.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void ContextSwitch(cinux::arch::CpuContext* outgoing, const cinux::arch::CpuContext* incoming);

/**
 * @brief         First instruction of every freshly built task.
 * @note          Never returns by design: enables interrupts, calls
 *                the entry held in r12, and on its return jumps to
 *                the hook held in r13, so a task that simply returns
 *                still lands in the unified exit path.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void TaskTrampoline();

}  // extern "C"
