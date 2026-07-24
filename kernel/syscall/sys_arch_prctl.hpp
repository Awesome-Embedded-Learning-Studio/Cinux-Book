/**
 * @file kernel/syscall/sys_arch_prctl.hpp
 * @brief sys_arch_prctl handler declaration (F10-M1 batch 4)
 *
 * arch_prctl is the x86-64 TLS syscall.  musl's static startup calls
 * arch_prctl(ARCH_SET_FS, tp) from __init_tp() to point %fs at the
 * thread's TLS block -- without it every %fs-relative access faults.
 * Cinux already wires the FS base through context_switch.S and
 * clone(CLONE_SETTLS); this handler reuses set_tls_base().
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// arch_prctl(code, addr) -- set/get the FS or GS segment base.
int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
