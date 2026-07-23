/**
 * @file kernel/arch/x86_64/fault_diag.hpp
 * @brief debugcon first-fault diagnostics (F4-M4 GOTCHA#25 / F-VERIFY M6)
 *
 * Declared separately from exception_handlers.cpp to keep that file under the
 * 500-line limit (CODING-TASTE).  Implementations in fault_diag.cpp.
 *
 * These dump the faulting frame to the QEMU debug console (port 0xE9, captured
 * to build/debug.log) BEFORE any code path that could recurse into another
 * fault (a corrupt %gs #GP, or a kprintf that itself #PFs).  The debugcon
 * channel always survives, so the FIRST fault's frame is preserved instead of
 * being overwritten by a recursive crash.
 */
#pragma once

#include <stdint.h>

namespace cinux::arch {

struct InterruptFrame;  // defined in idt.hpp (forward-declared to keep this light)

}  // namespace cinux::arch

// Dumps the first #GP's faulting frame to debugcon exactly once (recursive
// frames from a corrupt %gs are skipped).  Called at the top of handle_gp.
void capture_first_gp(const cinux::arch::InterruptFrame* frame);

// F-VERIFY M6-1: dumps the first #PF's frame (CR2 + decoded err P/W/U/RSV/I) to
// debugcon exactly once.  Called from handle_pf on PRESENT faults only.
void capture_first_pf(const cinux::arch::InterruptFrame* frame, uint64_t cr2);

// F-VERIFY M6-2: lock-free PTE walk that prints the CoW-fault backing phys + its
// mapcount to debugcon.  Called only on the CoW-resolution-FAIL path (rare).
void dump_cow_fail_diagnostic(uint64_t fault_addr);
