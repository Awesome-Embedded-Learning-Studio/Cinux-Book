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
 * as gs:0 / gs:8 / gs:16 / gs:24, and the asserts keep the C++ and
 * the asm from drifting apart.
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
 *                that must cross the frame teardown; syscall_frame
 *                banks where the entry frame sits, which fork reads
 *                for the whole user register image now that the frame
 *                carries every general register. All start zero and
 *                are written only by entry assembly. One CPU, one
 *                block today; the multicore station grows one per CPU
 *                and self() learns whose to hand out — the call sites
 *                never learn the difference.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
struct PerCpu : public cinux::base::Singleton<PerCpu> {
    unsigned long long kernel_rsp;     ///< gs:0, the syscall-entry stack.
    unsigned long long user_rsp;       ///< gs:8, the caller's stack, staged here.
    unsigned long long scratch;        ///< gs:16, one quadword of crossing room.
    unsigned long long syscall_frame;  ///< gs:24, where the entry frame sits.
};

static_assert(sizeof(PerCpu) == 32, "the GS block is four quadwords now");
static_assert(offsetof(PerCpu, kernel_rsp) == 0, "assembly reaches kernel_rsp at gs:0");
static_assert(offsetof(PerCpu, user_rsp) == 8, "assembly stages user_rsp at gs:8");
static_assert(offsetof(PerCpu, scratch) == 16, "assembly keeps its scratch at gs:16");
static_assert(offsetof(PerCpu, syscall_frame) == 24, "assembly banks the frame at gs:24");

/**
 * @brief         Point both GS bases at the per-CPU block.
 * @return        None.
 * @note          Bring-up only. IA32_GS_BASE is the one live MSR in
 *                every ring from here on — the block never leaves it,
 *                and the entry assembly addresses the block with no
 *                exchange at all. IA32_KERNEL_GSBASE starts on the
 *                block too, then becomes the staging slot the task
 *                switch moves each task's own GS base through.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void InstallKernelGs();

/**
 * @brief         Save the outgoing task's address-space bases.
 * @param[out]    fs_base    Receives the live FS base (MSR 0xC0000100).
 * @param[out]    gs_base    Receives the task's own GS base, read from
 *                           IA32_KERNEL_GSBASE where the switch staged
 *                           it.
 * @return        None.
 * @note          Call only while the outgoing task still holds the
 *                CPU, and only from kernel context — IA32_GS_BASE
 *                never changes rings, so the staging MSR is the sole
 *                home of a task's own GS value.
 * @since         0.8.0
 * @ingroup       kernel_arch
 */
void StoreTaskAddressRegs(unsigned long long& fs_base, unsigned long long& gs_base);

/**
 * @brief         Load the incoming task's address-space bases.
 * @param[in]     fs_base    The FS base to make live.
 * @param[in]     gs_base    The task's own GS base, staged into
 *                           IA32_KERNEL_GSBASE for the next save and
 *                           the entry path's benefit.
 * @return        None.
 * @note          The FS write takes effect for the running code at
 *                once; kernel code keeps off FS-relative addressing,
 *                so the outgoing task is unaffected until the switch.
 * @since         0.8.0
 * @ingroup       kernel_arch
 */
void LoadTaskAddressRegs(unsigned long long fs_base, unsigned long long gs_base);

/**
 * @brief         Make a value the live FS base.
 * @param[in]     base   The canonical address FS-relative addressing
 *                       resolves through from now on.
 * @return        None.
 * @note          The arch_prctl path's immediate write: the calling
 *                task is the one holding the CPU, so its context word
 *                and this MSR move together.
 * @since         0.8.0
 * @ingroup       kernel_arch
 */
void WriteFsBase(unsigned long long base);

/**
 * @brief         Stage a value as the task's own GS base.
 * @param[in]     base   The canonical address to track for this task.
 * @return        None.
 * @note          Lands in IA32_KERNEL_GSBASE, the staging MSR — the
 *                live IA32_GS_BASE stays on the per-CPU block in
 *                every ring, so the next switch saves exactly what
 *                this wrote.
 * @since         0.8.0
 * @ingroup       kernel_arch
 */
void WriteKernelGsBase(unsigned long long base);

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
