/**
 * @file kernel/arch/x86_64/tlb.cpp
 * @brief Cross-core TLB shootdown implementation (B3 defect C)
 */

#include "kernel/arch/x86_64/tlb.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging.hpp"        // flush_tlb
#include "kernel/arch/x86_64/smp.hpp"           // kShootdownIpiVector, online_ap_count
#include "kernel/drivers/apic/local_apic.hpp"   // g_lapic
#include "kernel/mm/pmm.hpp"                    // g_pmm.free_page (deferred fallback)
#include "kernel/mm/slab.hpp"                   // kmalloc/kfree (PendingShootdown nodes)
#include "kernel/proc/sync.hpp"                 // Spinlock, Semaphore

namespace cinux::arch {

namespace {

/// Single-in-flight shootdown state.  The sender sets vaddr + acks_remaining,
/// sends the IPI, local-invalidates, and spins until acks_remaining==0.  Each
/// receiver's shootdown_ipi_handler reads vaddr + decrements acks.
struct ShootdownState {
    proc::Spinlock lock;
    uint64_t       vaddr          = 0;
    uint64_t       acks_remaining = 0;
};

ShootdownState g_shootdown;

}  // namespace

void tlb_shootdown_page(uint64_t vaddr) {
    auto guard = g_shootdown.lock.guard();  // serialize concurrent callers (single in-flight)

    g_shootdown.vaddr          = vaddr & ~0xFFFULL;  // page-align
    g_shootdown.acks_remaining = online_ap_count();  // excludes BSP; matches all-excl-self

    // Local CPU invalidates its own TLB for this vaddr; we do not IPI self.
    flush_tlb(g_shootdown.vaddr);

    if (g_shootdown.acks_remaining == 0) {
        return;  // single-core or no APs online -> nothing to wait for
    }

    // The vaddr store above must be visible to receivers before they handle
    // the IPI.  On x86 TSO the LAPIC ICR write (MMIO) is ordered after prior
    // stores, and the receiver reads vaddr only after the IPI lands, so no
    // extra fence is needed.
    drivers::apic::g_lapic.send_ipi_all_others(kShootdownIpiVector);

    // Spin (ACQUIRE) until every receiver has acked.  Bounded by the longest
    // IF=0 section another CPU can be in: it services the IPI on exit.
    while (__atomic_load_n(&g_shootdown.acks_remaining, __ATOMIC_ACQUIRE) != 0) {
        __asm__ volatile("pause");
    }
}

extern "C" void shootdown_ipi_handler(InterruptFrame* /*frame*/) {
    // The ISR_IRQ stub owns the EOI; this handler just invalidates + acks.
    // RELAXED read of vaddr: the sender's store preceded the IPI send (see
    // tlb_shootdown_page), and on this CPU the IPI delivery is ordered before
    // the handler's reads.
    flush_tlb(__atomic_load_n(&g_shootdown.vaddr, __ATOMIC_RELAXED));
    __atomic_sub_fetch(&g_shootdown.acks_remaining, 1, __ATOMIC_ACQ_REL);
}

// ============================================================
// Deferred shootdown + free (stage2, B3 defect C)
// ============================================================
// handle_cow_fault (#PF, IF=0) cannot do a sync shootdown (deadlock with a
// peer CoW-faulting the same page).  It pushes (phys, vaddr) here; the drain
// kthread (IF=1) does shootdown + free.  g_drain_active gates deferred vs
// inline-free: false (suite-only test kernel / pre-init / OFF) -> free inline
// with no shootdown, matching the pre-deferred single-core behaviour.

proc::Semaphore g_pending_sem(0);
bool            g_drain_active = false;
namespace {
proc::Spinlock    g_pending_lock;
PendingShootdown* g_pending_head = nullptr;
}  // namespace

void enqueue_pending_shootdown(uint64_t phys, uint64_t vaddr) {
    if (!__atomic_load_n(&g_drain_active, __ATOMIC_ACQUIRE)) {
        // Drain kthread not running: free inline, no cross-core shootdown.
        // Single-core or APs-idle -- the local flush_tlb in handle_cow_fault
        // already covered this CPU.
        cinux::mm::g_pmm.free_page(phys);
        return;
    }
    auto* node = static_cast<PendingShootdown*>(
        cinux::mm::kmalloc(sizeof(PendingShootdown), alignof(PendingShootdown)));
    if (node == nullptr) {
        // OOM: degrade to free-without-shootdown (defect C stale RO read, not
        // corruption).  Rare; the page is still freed.
        cinux::mm::g_pmm.free_page(phys);
        return;
    }
    node->phys  = phys;
    node->vaddr = vaddr & ~0xFFFULL;
    {
        // Producer may run at IF=0 (#PF); irq_guard is a no-op-on-IF there and
        // serializes vs the drain kthread's dequeue.
        auto g = g_pending_lock.irq_guard();
        node->next     = g_pending_head;
        g_pending_head = node;
    }
    // post() uses plain guard (no IF change) + Scheduler::unblock (run-queue
    // edit, no schedule) -- safe at IF=0.
    g_pending_sem.post();
}

PendingShootdown* dequeue_pending_shootdown() {
    auto g = g_pending_lock.irq_guard();
    PendingShootdown* node = g_pending_head;
    if (node != nullptr) {
        g_pending_head = node->next;
    }
    return node;
}

}  // namespace cinux::arch
