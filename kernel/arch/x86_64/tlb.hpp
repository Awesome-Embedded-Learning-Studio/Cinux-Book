/**
 * @file kernel/arch/x86_64/tlb.hpp
 * @brief Cross-core TLB shootdown (B3 defect C)
 *
 * tlb_shootdown_page() invalidates a virtual address on every other CPU via an
 * all-excluding-self IPI (vector kShootdownIpiVector), then waits for each
 * receiver to ack.  The sender local-invalidates itself and does not IPI self.
 *
 * SINGLE IN-FLIGHT: g_shootdown holds one vaddr at a time, so concurrent
 * callers serialize behind g_shootdown.lock.  Stage 2's drain kthread is the
 * only caller and processes pending entries serially, so this is fine; a
 * future caller wanting concurrent shootdowns would need per-vaddr state.
 *
 * The handler (shootdown_ipi_handler) is called from the ISR_IRQ stub which
 * owns the EOI -- the handler does NOT call eoi() itself.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

struct InterruptFrame;  // defined in idt.hpp

/// Invalidate @p vaddr on every other CPU (all-excl-self IPI), then wait for
/// each to ack.  The sender local-invalidates first and does not IPI self.
/// Safe to call at IF=0; the spin on acks_remaining is bounded by the longest
/// IF=0 section another CPU can be in (it services the IPI on exit).  No-op
/// (skips IPI + spin) when online_ap_count()==0.
void tlb_shootdown_page(uint64_t vaddr);

/// Receiver side of tlb_shootdown_page, entered from shootdown_ipi_stub
/// (ISR_IRQ).  invlpg's the sender's vaddr + atomically decrements acks.
/// The stub owns the EOI.  extern "C" so the asm stub can call it by name.
extern "C" void shootdown_ipi_handler(InterruptFrame* frame);

}  // namespace cinux::arch
