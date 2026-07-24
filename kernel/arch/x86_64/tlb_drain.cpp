/**
 * @file kernel/arch/x86_64/tlb_drain.cpp
 * @brief TLB shootdown drain kthread (B3 defect C, stage2)
 *
 * Spawns a resident kthread that drains the pending-shootdown list: for each
 * (phys, vaddr) pushed by handle_cow_fault it runs the sync tlb_shootdown_page
 * (safe here -- the kthread is at IF=1, NOT inside a #PF) then frees the page.
 * This is the deferred half of defect C: handle_cow_fault pushes instead of
 * freeing so the free never races another core's stale TLB.
 *
 * Why no deadlock (vs the naive sync-in-CoW design): the drain kthread holds
 * no lock that handle_cow_fault needs (PMM lock is short, released before the
 * push), and a target CPU in handle_cow_fault (IF=0) services the shootdown
 * IPI as soon as #PF returns and IF restores -- so the spin terminates.
 *
 * NOT sti/hlt: Semaphore::wait blocks by scheduling out (stats_kthread §14
 * warns sti/hlt in a band-0 kthread starves every other band-0 task under a
 * tick).  priority 0 so the deferred-free backlog drains promptly.
 *
 * Built when CINUX_TLB_DRAIN=ON (default); OFF links tlb_drain_stub.cpp.
 * Namespace: cinux::arch
 */

#include "kernel/arch/x86_64/tlb.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task_builder.hpp"

namespace cinux::arch {

using cinux::lib::kprintf;

void tlb_drain_entry() {
    kprintf("[TLB] drain kthread entry\n");
    while (true) {
        g_pending_sem.wait();  // blocks (schedule out) until a push posts
        PendingShootdown* node;
        while ((node = dequeue_pending_shootdown()) != nullptr) {
            tlb_shootdown_page(node->vaddr);       // sync: IPI all-excl-self + spin acks
            cinux::mm::g_pmm.free_page(node->phys);  // NOW safe to free
            cinux::mm::kfree(node);
        }
    }
}

void start_tlb_drain_thread() {
    // Flip enqueue to deferred BEFORE spawning so the first push after this
    // goes to the list (not inline free).
    __atomic_store_n(&g_drain_active, true, __ATOMIC_RELEASE);
    auto* t = cinux::proc::TaskBuilder()
                  .set_entry(tlb_drain_entry)
                  .set_name("tlb_drain")
                  .set_priority(0)  // band 0: drain promptly to bound the stale window
                  .build();
    if (t != nullptr) {
        cinux::proc::Scheduler::add_task(t);
        kprintf("[TLB] drain kthread started\n");
    } else {
        kprintf("[TLB] drain kthread spawn FAILED -- staying inline\n");
        __atomic_store_n(&g_drain_active, false, __ATOMIC_RELEASE);
    }
}

}  // namespace cinux::arch
