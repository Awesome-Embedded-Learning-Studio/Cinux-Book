/**
 * @file kernel/arch/x86_64/tlb.hpp
 * @brief Cross-core TLB shootdown (B3 defect C)
 *
 * tlb_shootdown_page() invalidates a virtual address on every other CPU via an
 * all-excluding-self IPI (vector kShootdownIpiVector), then waits for each
 * receiver to ack.  The sender local-invalidates itself and does not IPI self.
 *
 * SINGLE IN-FLIGHT: g_shootdown holds one vaddr at a time, so concurrent
 * callers serialize behind g_shootdown.lock.  The drain kthread is the only
 * caller and processes pending entries serially, so this is fine; a future
 * caller wanting concurrent shootdowns would need per-vaddr state.
 *
 * The handler (shootdown_ipi_handler) is called from the ISR_IRQ stub which
 * owns the EOI -- the handler does NOT call eoi() itself.
 *
 * DEFERRED (stage2): handle_cow_fault runs at IF=0 (#PF) so it CANNOT do a
 * sync shootdown directly -- two CPUs CoW-faulting the same page would
 * deadlock (A holds the shootdown lock spinning for B's ack while B spins on
 * the lock at IF=0, unable to take A's IPI).  Instead it pushes (phys, vaddr)
 * via enqueue_pending_shootdown(); a drain kthread at IF=1 does the sync
 * shootdown + free.  When the drain kthread is not running (g_drain_active
 * false -- suite-only test kernel or pre-init), enqueue falls back to an
 * inline free_page (no shootdown: single-core-safe, matches pre-deferred).
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

#include "kernel/proc/sync.hpp"  // Semaphore

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

// ============================================================
// Deferred shootdown + free (stage2, B3 defect C)
// ============================================================
struct PendingShootdown {
    uint64_t          phys;
    uint64_t          vaddr;
    PendingShootdown* next;
};

/// Push a deferred shootdown+free for @p phys (whose pte_count/refcount just
/// hit 0 via pte_count_dec_and_test_no_free).  When the drain kthread is
/// active the page is freed later (after shootdown); otherwise freed inline
/// here (no shootdown -- single-core / pre-init fallback).
void enqueue_pending_shootdown(uint64_t phys, uint64_t vaddr);

/// Spawn the TLB drain kthread (production only -- Semaphore::wait needs the
/// scheduler).  Sets g_drain_active so enqueue stops deferring.  Empty stub
/// when CINUX_TLB_DRAIN=OFF (then enqueue always inline-frees).
void start_tlb_drain_thread();

// Internal -- consumed by tlb_drain.cpp.
extern proc::Semaphore g_pending_sem;
extern bool            g_drain_active;
PendingShootdown* dequeue_pending_shootdown();

}  // namespace cinux::arch
