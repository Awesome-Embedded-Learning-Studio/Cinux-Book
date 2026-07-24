/**
 * @file kernel/arch/x86_64/tlb.cpp
 * @brief Cross-core TLB shootdown implementation (B3 defect C)
 */

#include "kernel/arch/x86_64/tlb.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging.hpp"        // flush_tlb
#include "kernel/arch/x86_64/smp.hpp"           // kShootdownIpiVector, online_ap_count
#include "kernel/drivers/apic/local_apic.hpp"   // g_lapic
#include "kernel/proc/sync.hpp"                 // Spinlock

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

}  // namespace cinux::arch
