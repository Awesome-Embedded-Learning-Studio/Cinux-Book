/**
 * @file    proc_config.hpp
 * @brief   Tunable facts of kernel threading.
 *
 * The facts of kernel threading, one constant each: how many pages
 * one kernel-thread stack spans, the marker written at its bottom as
 * a post-mortem overflow probe, how many tasks may wait in one
 * scheduling class's ready row, and how the scheduler's slice and
 * class registry are sized. Overflow of a ready row is a Check
 * failure, not a silent drop — losing a wakeup is undebuggable,
 * failing loudly is one serial line.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/literal_types.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/layout.hpp"

namespace cinux::proc {

using cinux::base::operator""_MiB;

/// Pages of physical memory behind one kernel-thread stack.
inline constexpr unsigned int kStackPages = 4;

/// log2 of kStackPages; the PMM frees with the same order it granted.
inline constexpr int kStackOrder = 2;

/// Marker at the very bottom of every thread stack; changed means the
/// stack ran past its end.
inline constexpr unsigned long long kStackMagic = 0xDEADC0DEULL;

/// Tasks that may wait in one class's ready row at once; every
/// scheduling class owns a row of this size.
inline constexpr unsigned int kReadyQueueCapacity = 16;

/// Timer ticks one task may run before the scheduler asks for a
/// rotation; two at 100 Hz is 20 ms per slice.
inline constexpr unsigned int kTimeSliceTicks = 2;

/// Scheduling classes the scheduler accepts registrations from.
inline constexpr unsigned int kMaxSchedulingClasses = 4;

/// Bytes of one kernel-thread stack.
inline constexpr unsigned long kStackBytes =
    static_cast<unsigned long>(kStackPages) * cinux::arch::page::kSize;

static_assert((1UL << static_cast<unsigned long>(kStackOrder)) == kStackPages,
              "order and page count are one fact");

/// Where user programs park their stack: the address-space layout's own
/// stack top, restated here so the proc theme names one fact once.
inline constexpr unsigned long kUserProgramStackTop = mm::kUserStackTop;

/// How far above the mapped stack page's base a fresh user world's
/// first stack pointer lands: the whole entry picture — argc, argv,
/// envp, auxv words and every string they name — then sits above that
/// pointer inside the page, and everything below it stays empty for
/// the call stack to grow into. Laying the strings below the pointer
/// instead once let any ordinary call overwrite them: argv[0] and PATH
/// died to returning addresses, and a shell lost its own name mid-
/// session. ELF process entry requires a sixteen-byte-aligned stack;
/// freestanding programs enter C++ through their assembly startup stub.
inline constexpr unsigned long kUserEntryStackOffset = 0x200;

/// Resource rows prlimit64 can name; the uapi source stops at row 15
/// (RLIMIT_RTTIME) and one past it closes the table.
inline constexpr unsigned int kResourceLimitCount = 16;

/** @brief Linux RLIMIT_STACK resource index. */
inline constexpr unsigned int kResourceStack = 3;

/// The ceiling word that answers "no ceiling": RLIM_INFINITY in the
/// same uapi table.
inline constexpr unsigned long long kResourceLimitInfinity = ~0ULL;

/// Soft stack ceiling a fresh task is seated with; the uapi source
/// spells it _STK_LIM.
inline constexpr unsigned long long kDefaultStackCeiling = 8_MiB;

/// Open-file rows a fresh task is seated with: what one task may hold,
/// and where the hard ceiling sits; the uapi source spells them
/// INR_OPEN_CUR and INR_OPEN_MAX.
inline constexpr unsigned long long kDefaultOpenFiles = 1024;
inline constexpr unsigned long long kMaxOpenFiles     = 4096;

/// The ceilings a fresh task is seated with, one {soft, hard} row per
/// resource: stack 8 MiB soft (the uapi _STK_LIM), no core dumps, and
/// 1024/4096 open files; everything else answers unlimited.
inline constexpr unsigned long long kDefaultResourceLimits[kResourceLimitCount][2] = {
    {kResourceLimitInfinity, kResourceLimitInfinity},  // CPU
    {kResourceLimitInfinity, kResourceLimitInfinity},  // FSIZE
    {kResourceLimitInfinity, kResourceLimitInfinity},  // DATA
    {kDefaultStackCeiling, kResourceLimitInfinity},    // STACK
    {0, kResourceLimitInfinity},                       // CORE
    {kResourceLimitInfinity, kResourceLimitInfinity},  // RSS
    {kResourceLimitInfinity, kResourceLimitInfinity},  // NPROC
    {kDefaultOpenFiles, kMaxOpenFiles},                // NOFILE
    {kResourceLimitInfinity, kResourceLimitInfinity},  // MEMLOCK
    {kResourceLimitInfinity, kResourceLimitInfinity},  // AS
    {kResourceLimitInfinity, kResourceLimitInfinity},  // LOCKS
    {kResourceLimitInfinity, kResourceLimitInfinity},  // SIGPENDING
    {kResourceLimitInfinity, kResourceLimitInfinity},  // MSGQUEUE
    {kResourceLimitInfinity, kResourceLimitInfinity},  // NICE
    {kResourceLimitInfinity, kResourceLimitInfinity},  // RTPRIO
    {kResourceLimitInfinity, kResourceLimitInfinity},  // RTTIME
};

}  // namespace cinux::proc
