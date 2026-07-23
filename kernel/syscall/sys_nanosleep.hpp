/**
 * @file kernel/syscall/sys_nanosleep.hpp
 * @brief sys_nanosleep handler declaration (F-ECO batch 3)
 *
 * Sleep for a timespec duration.  Backed by the HPET monotonic counter (PIT
 * fallback): the sleep poll+yields until the deadline.  Two deliberate,
 * documented hobby-OS simplifications:
 *   - NO sti/hlt: yield() context-switches via Task::ctx (the #DF-safe path,
 *     same as ping's pump_yield).  sti inside a syscall re-enables the LAPIC
 *     timer IRQ and corrupts the syscall trap frame -> #DF.
 *   - NO IRQ wake: a timer-interrupt-driven wake (HPET 周期中断) is the F5-M4
 *     follow-up.  Until then the loop busy-polls the counter with yield() giving
 *     other tasks the CPU -- correct, just not efficient.
 * Signal-interruption (-EINTR + remaining time) is not delivered: CinuxOS does
 * not wake nanosleep on signal, so a requested sleep always completes and @p rem
 * is zeroed.  busybox `sleep` (whole-second) works; sub-second + EINTR later.
 *
 * Reuses ktimespec from sys_clock_gettime.hpp (the kernel timespec layout).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/syscall/sys_clock_gettime.hpp"  // ktimespec

namespace cinux::syscall {

/// nanosleep(req, rem) -- sleep for the requested duration; returns 0.
int64_t sys_nanosleep(uint64_t req_virt, uint64_t rem_virt, uint64_t, uint64_t, uint64_t, uint64_t);

/// P0e (SMAP): sleep using KERNEL ktimespec pointers (no user memory).
/// Tests call this; sys_nanosleep is the user boundary (copy_to/from_user).
int64_t do_nanosleep_kernel(const ktimespec* req, ktimespec* rem);

}  // namespace cinux::syscall
