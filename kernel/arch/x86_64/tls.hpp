/**
 * @file kernel/arch/x86_64/tls.hpp
 * @brief Thread-Local Storage (TLS) base register helpers (F3-M2 batch 1)
 *
 * x86-64 implements per-thread TLS via the FS segment base (MSR_FS_BASE).
 * Each thread owns an independent FS base pointing at its TLS block; the
 * value is saved/restored in CpuContext across context switches
 * (context_switch.S) so every task sees its own %fs-relative data.
 *
 * set_tls_base() is invoked from clone(CLONE_SETTLS); get_tls_base() is
 * a diagnostic/query helper.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

/// MSR index for the FS segment base (per-thread TLS pointer).
constexpr uint32_t kMsrFsBase = 0xC0000100;

/**
 * @brief Set the current thread's TLS base (FS segment base)
 *
 * Writes @p addr into MSR_FS_BASE.  Subsequent %fs-relative memory
 * accesses use the new base.  The value is captured into CpuContext on
 * the next context switch, so each task keeps its own TLS pointer.
 *
 * @p addr must be a canonical x86-64 address (bits 48..63 sign-extend
 * bit 47); a non-canonical value raises #GP on the wrmsr.  Real TLS
 * bases are user pointers and thus always canonical, so clone's
 * CLONE_SETTLS argument is safe.
 *
 * @param addr  Virtual address of the TLS block (0 to clear TLS)
 */
void set_tls_base(uint64_t addr);

/**
 * @brief Read the current thread's TLS base
 *
 * @return The current MSR_FS_BASE value
 */
uint64_t get_tls_base();

}  // namespace cinux::arch
