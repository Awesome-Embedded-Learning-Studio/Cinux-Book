/**
 * @file    msr.hpp
 * @brief   Model-specific register numbers, spelled once.
 *
 * The numbers are architecture facts with no derivation: a value
 * transcribed one digit off compiles happily and detonates much
 * later, which is exactly how the FS/GS/KERNEL_GS trio once got
 * rotated by one and a swapgs pulled a zero. So the values live in
 * exactly one place, named the way Linux's msr-index.h names them,
 * and every consumer reaches for a word instead of a hex literal.
 * Bits inside a register that several files care about (EFER's, so
 * far) live here beside their register.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::arch::msr {

/// EFER, the extended-feature register long mode lives in.
inline constexpr unsigned int kEfer = 0xC0000080;

/// STAR, the selector pair SYSCALL and SYSRET read.
inline constexpr unsigned int kStar = 0xC0000081;

/// LSTAR, the target RIP of SYSCALL.
inline constexpr unsigned int kLstar = 0xC0000082;

/// SFMASK, the RFLAGS bits SYSCALL clears on entry.
inline constexpr unsigned int kSfmask = 0xC0000084;

/// FS base, the thread pointer FS-relative addressing resolves through.
inline constexpr unsigned int kFsBase = 0xC0000100;

/// GS base, the one active in ring 0 before any swapgs.
inline constexpr unsigned int kGsBase = 0xC0000101;

/// KERNEL_GSBASE, the swapgs partner the syscall entry lands on.
inline constexpr unsigned int kKernelGsBase = 0xC0000102;

/// EFER bit 0: SYSCALL and SYSRET leave their disabled state.
inline constexpr unsigned long long kEferSyscallEnable =
    cinux::base::bit::MaskBit<unsigned long long>(0).raw;

/// EFER bit 8: long mode is enabled.
inline constexpr unsigned long long kEferLongModeEnable =
    cinux::base::bit::MaskBit<unsigned long long>(8).raw;

/// EFER bit 10: long mode is active.
inline constexpr unsigned long long kEferLongModeActive =
    cinux::base::bit::MaskBit<unsigned long long>(10).raw;

/// EFER bit 11: the no-execute page bit is honored.
inline constexpr unsigned long long kEferNoExecuteEnable =
    cinux::base::bit::MaskBit<unsigned long long>(11).raw;

}  // namespace cinux::arch::msr
