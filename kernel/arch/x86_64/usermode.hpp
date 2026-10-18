/**
 * @file    usermode.hpp
 * @brief   The one-way door into ring 3.
 *
 * Before syscalls exist there is no road back, so the first visit to
 * user mode borrows the return half of the SYSRET path: point RCX at
 * the first user instruction, R11 at the flags to land with, and let
 * one sysretq carry the CPU down to DPL 3. The STAR base was chosen in
 * gdt.hpp so the selectors SYSRETQ computes already carry RPL 3; this
 * file only writes the MSR pair and performs the jump. The caller owns
 * the address space and the stacks — this door just opens.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch::usermode {

/**
 * @brief         Unlock the fast-system instructions and aim STAR.
 * @return        None.
 * @note          EFER.SCE gates both SYSCALL and SYSRET — without it
 *                the instructions themselves fault as invalid opcodes,
 *                whichever direction you face. With it on, SYSCALL
 *                entries read STAR[47:32] for their kernel code
 *                selector and +8 for data, while SYSRET reads
 *                STAR[63:48], the value that already folds RPL 3 into
 *                itself.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void EnableFastSystemCalls();

}  // namespace cinux::arch::usermode

extern "C" {

/**
 * @brief         Leave ring 0 through sysretq and never come back.
 * @param[in]     entry     First user instruction, in the user half.
 * @param[in]     entry_rsp The exact RSP the first instruction finds;
 *                          the caller owns the entry stack's alignment
 *                          and whatever picture (argc, argv, envp) sits
 *                          on top of it.
 * @return        None, ever.
 * @note          The CPU lands with CS 0x33, SS 0x2B, RFLAGS from the
 *                constant in usermode.S (interrupts enabled), and RSP
 *                verbatim from entry_rsp. C linkage, like the context
 *                switch below it — assembly spells one name.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
[[noreturn]] void JumpToRing3(unsigned long long entry, unsigned long long entry_rsp);

}  // extern "C"
