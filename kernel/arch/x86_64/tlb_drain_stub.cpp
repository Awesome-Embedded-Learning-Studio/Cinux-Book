/**
 * @file kernel/arch/x86_64/tlb_drain_stub.cpp
 * @brief Empty start_tlb_drain_thread() when CINUX_TLB_DRAIN=OFF (stage2 gate)
 *
 * Source has no #ifdef; CMake links this TU instead of tlb_drain.cpp when the
 * option is off, so init.cpp can call start_tlb_drain_thread() unconditionally.
 * g_drain_active stays false -> enqueue_pending_shootdown always inline-frees
 * (defect C stale tolerated, no shootdown -- the pre-deferred behaviour).
 *
 * Namespace: cinux::arch
 */

#include "kernel/arch/x86_64/tlb.hpp"

namespace cinux::arch {

void start_tlb_drain_thread() {}

}  // namespace cinux::arch
