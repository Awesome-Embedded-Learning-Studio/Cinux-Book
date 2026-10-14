/**
 * @file    per_cpu.hpp
 * @brief   The per-CPU scratch block the kernel reaches through GS.
 *
 * The syscall entry and swapgs live in a world where %gs-relative
 * addressing is the only addressing that survives a privilege hop:
 * segment overrides keep working when the page tables switch, so the
 * CPU's GS base is the one pointer the kernel never has to reload.
 * This block is what it points at. Today one CPU owns one static
 * block; the multicore station grows one per CPU and the accessor
 * follows. Field offsets are load-bearing — assembly addresses them
 * as gs:0 / gs:8 / gs:16, and the asserts keep the C++ and the asm
 * from drifting apart.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>

#include "cinux/singleton.hpp"

namespace cinux::arch::per_cpu {

/**
 * @brief         The scratch quadwords behind GS.
 * @note          kernel_rsp is the ring-0 stack the syscall entry
 *                loads; user_rsp holds the caller's stack across that
 *                switch; scratch is the one-slot lifeline for values
 *                that must cross the frame teardown; syscall_frame and
 *                user_r12..r15 bank what fork needs from the moment of
 *                the call — the entry frame's address and the four
 *                callee-saved registers the minimal frame does not
 *                push, kept the only way they can be, before any
 *                kernel code borrows those registers for itself. All
 *                start zero and are written only by entry assembly.
 *                One CPU, one block today; the multicore station
 *                grows one per CPU and self() learns whose to hand
 *                out — the call sites never learn the difference.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
struct PerCpu : public cinux::base::Singleton<PerCpu> {
    unsigned long long kernel_rsp;     ///< gs:0, the syscall-entry stack.
    unsigned long long user_rsp;       ///< gs:8, the caller's stack, staged here.
    unsigned long long scratch;        ///< gs:16, one quadword of crossing room.
    unsigned long long syscall_frame;  ///< gs:24, where the entry frame sits.
    unsigned long long user_r12;       ///< gs:32, the caller's r12 at entry.
    unsigned long long user_r13;       ///< gs:40, the caller's r13 at entry.
    unsigned long long user_r14;       ///< gs:48, the caller's r14 at entry.
    unsigned long long user_r15;       ///< gs:56, the caller's r15 at entry.
};

static_assert(sizeof(PerCpu) == 64, "the GS block is eight quadwords now");
static_assert(offsetof(PerCpu, kernel_rsp) == 0, "assembly reaches kernel_rsp at gs:0");
static_assert(offsetof(PerCpu, user_rsp) == 8, "assembly stages user_rsp at gs:8");
static_assert(offsetof(PerCpu, scratch) == 16, "assembly keeps its scratch at gs:16");
static_assert(offsetof(PerCpu, syscall_frame) == 24, "assembly banks the frame at gs:24");
static_assert(offsetof(PerCpu, user_r12) == 32, "assembly banks user r12 at gs:32");
static_assert(offsetof(PerCpu, user_r13) == 40, "assembly banks user r13 at gs:40");
static_assert(offsetof(PerCpu, user_r14) == 48, "assembly banks user r14 at gs:48");
static_assert(offsetof(PerCpu, user_r15) == 56, "assembly banks user r15 at gs:56");

/**
 * @brief         Point both GS bases at the per-CPU block.
 * @return        None.
 * @note          IA32_GS_BASE (the one live in ring 0) and
 *                IA32_KERNEL_GSBASE (the one swapgs exchanges it for)
 *                both receive the block's address. Until user programs
 *                ask for a TLS of their own the two are allowed to
 *                agree — swapgs then swaps equals, and every %gs
 *                reference in the kernel lands on this block whichever
 *                way the exchange went.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void InstallKernelGs();

/**
 * @brief         Read back the address GS currently resolves to.
 * @return        The IA32_GS_BASE value.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
unsigned long long ReadGsBase();

/**
 * @brief         Read back the swapgs partner of the GS base.
 * @return        The IA32_KERNEL_GSBASE value.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
unsigned long long ReadKernelGsBase();

}  // namespace cinux::arch::per_cpu
