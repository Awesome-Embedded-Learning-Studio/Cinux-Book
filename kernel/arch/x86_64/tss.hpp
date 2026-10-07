/**
 * @file    tss.hpp
 * @brief   The task-state segment the kernel owns but rarely touches.
 *
 * In long mode the TSS is not a hardware task switch — nobody schedules
 * through it. It earns its slot for one field: RSP0, the stack the CPU
 * loads when an interrupt arrives from ring 3. Before user mode exists
 * interrupts only ever land on the current stack, which is why this
 * struct could wait for its station. The layout below is the
 * architecture's own: offsets are not ours to choose, and the
 * static_asserts pin every field the switcher ever cares about.
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

#include "cinux/singleton.hpp"

namespace cinux::arch::tss {

/**
 * @brief         The 104-byte task-state segment of x86_64.
 * @note          Only rsp0 has a consumer today: privilege changes
 *                from ring 3 load it as the new RSP. The ist array is
 *                left zero — interrupt stacks are a deliberate future,
 *                not a default. Reserved fields stay zero because the
 *                CPU reads the whole struct and zero is the only value
 *                it has ever defined for them. Single-core truth: one
 *                CPU, one TSS; the multicore station grows one per CPU
 *                and self() learns whose to hand out — the shape
 *                survives, and so does the static_assert fence below.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
struct [[gnu::packed]] Tss : public cinux::base::Singleton<Tss> {
    unsigned int       front;          ///< Offset 0x00, architecture reserved.
    unsigned long long rsp0;           ///< Offset 0x04, the ring-0 stack for ring-3 entries.
    unsigned long long rsp1;           ///< Offset 0x0C, unused outside nested privilege levels.
    unsigned long long rsp2;           ///< Offset 0x14, unused outside nested privilege levels.
    unsigned long long filler;         ///< Offset 0x1C, architecture reserved.
    unsigned long long ist[7];         ///< Offset 0x24, interrupt stacks, parked at zero.
    unsigned short     tail[5];        ///< Offset 0x5C, architecture reserved.
    unsigned short     io_bitmap_end;  ///< Offset 0x66, one past the (absent) I/O bitmap.
};

static_assert(sizeof(Tss) == 104, "the 64-bit TSS is 0x68 bytes, empty base included");
static_assert(offsetof(Tss, rsp0) == 0x04, "RSP0 lives at 0x04");
static_assert(offsetof(Tss, ist) == 0x24, "the IST array starts at 0x24");
static_assert(offsetof(Tss, io_bitmap_end) == 0x66, "the I/O bitmap offset lives at 0x66");

/**
 * @brief         Point the task register at the boot TSS.
 * @return        None.
 * @note          ltr with selector 0x18; the descriptor was already
 *                encoded by LoadOwnedGdt. Safe only while interrupts
 *                are off, which is also when every bring-up runs.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void LoadTaskRegister();

/**
 * @brief         Read the task register back.
 * @return        The selector currently loaded into TR.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
unsigned short ReadTaskRegister();

}  // namespace cinux::arch::tss
