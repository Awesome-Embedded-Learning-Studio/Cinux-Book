/**
 * @file kernel/proc/lockdep.cpp
 * @brief Runtime lock-order deadlock detector implementation (F4-M5 R6-Part2)
 *
 * Built only under CINUX_LOCKDEP.  See lockdep.hpp for the model.  When
 * CINUX_LOCKDEP is off this translation unit is empty; callers use the inline
 * no-op stubs from the header.
 */

#ifdef CINUX_LOCKDEP

#    include "kernel/proc/lockdep.hpp"

#    include <stdint.h>

#    include "kernel/lib/kprintf.hpp"
#    include "kernel/proc/percpu.hpp"

namespace cinux::proc {

namespace {

constexpr int kMaxHeld  = 16;   ///< per-CPU held-lock stack depth
constexpr int kMaxEdges = 256;  ///< global lock-order graph edges
constexpr int kMaxDfs   = 64;   ///< visited-node bound for the cycle DFS

// Per-CPU stack of spinlock addresses currently held on that CPU.  Indexed by
// percpu()->cpu_id (a single global depth counter, as Part1 used, is wrong on
// SMP -- two CPUs would mutate one counter).
struct HeldStack {
    const void* held[kMaxHeld];
    int         depth;
};
HeldStack g_held[kMaxCpus] = {};

// Lock-order graph: edge {from, to} means `from` was held when `to` was
// acquired, i.e. the established lock order is from-then-to.
struct Edge {
    const void* from;
    const void* to;
};
Edge         g_edges[kMaxEdges];
uint32_t     g_edge_count = 0;
volatile int g_edge_lock  = 0;  // raw irq-safe spin -- NOT a Spinlock (no recursion)

// IRQ-safe raw spin guarding the edge table.  Held only for the brief cycle
// check + edge insert.  Disabling IRQs prevents same-CPU re-entry: an IRQ whose
// handler takes a Spinlock would call lockdep_acquired -> edge_lock_acquire and
// deadlock against this CPU's own hold.  Cross-CPU contention serializes via
// the atomic test-and-set.
void edge_lock_acquire(uint64_t& saved_flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=rm"(saved_flags));
    while (__atomic_test_and_set(&g_edge_lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

void edge_lock_release(uint64_t saved_flags) {
    __atomic_clear(&g_edge_lock, __ATOMIC_RELEASE);
    __asm__ volatile("pushq %0; popfq" : : "rm"(saved_flags));
}

// Does a path from->to exist over g_edges?  Bounded DFS; on overflow (more than
// kMaxDfs reachable locks) it stops expanding -- conservative: it may miss a
// long cycle but never false-positives.  Caller holds g_edge_lock.
bool reaches(const void* from, const void* to) {
    if (from == to) {
        return true;
    }
    const void* visited[kMaxDfs];
    int         nvis = 0;
    const void* stack[kMaxDfs];
    int         sp  = 0;
    stack[sp++]     = from;
    visited[nvis++] = from;
    while (sp > 0) {
        const void* cur = stack[--sp];
        for (uint32_t i = 0; i < g_edge_count; i++) {
            if (g_edges[i].from != cur) {
                continue;
            }
            const void* nxt = g_edges[i].to;
            if (nxt == to) {
                return true;
            }
            bool vis = false;
            for (int j = 0; j < nvis; j++) {
                if (visited[j] == nxt) {
                    vis = true;
                    break;
                }
            }
            if (!vis && nvis < kMaxDfs && sp < kMaxDfs) {
                visited[nvis++] = nxt;
                stack[sp++]     = nxt;
            }
        }
    }
    return false;
}

bool has_edge(const void* from, const void* to) {
    for (uint32_t i = 0; i < g_edge_count; i++) {
        if (g_edges[i].from == from && g_edges[i].to == to) {
            return true;
        }
    }
    return false;
}

}  // namespace

void lockdep_acquired(const void* lock) {
    uint32_t cpu = percpu()->cpu_id;
    if (cpu >= kMaxCpus) {
        return;  // pre-percpu boot (GS not yet anchored): skip
    }
    HeldStack& c = g_held[cpu];

    // Self-deadlock: acquiring a lock already held on this CPU.
    for (int i = 0; i < c.depth; i++) {
        if (c.held[i] == lock) {
            cinux::lib::kpanic("lockdep: recursive spinlock acquire %p", lock);
        }
    }

    // Cycle check + edge insert under the edge lock.  Acquiring `lock` while
    // holding held[i] establishes order held[i]->lock; a cycle exists if a path
    // lock->...->held[i] is already in the graph (lock was ordered before it).
    uint64_t sf;
    edge_lock_acquire(sf);
    for (int i = 0; i < c.depth; i++) {
        if (reaches(lock, c.held[i])) {
            edge_lock_release(sf);
            cinux::lib::kpanic(
                "lockdep: lock-order cycle -- acquiring %p while holding %p (AB-BA deadlock)", lock,
                c.held[i]);
        }
    }
    for (int i = 0; i < c.depth; i++) {
        if (!has_edge(c.held[i], lock) && g_edge_count < kMaxEdges) {
            g_edges[g_edge_count++] = {c.held[i], lock};
        }
    }
    edge_lock_release(sf);

    // Push onto this CPU's held stack.
    if (c.depth >= kMaxHeld) {
        cinux::lib::kpanic("lockdep: per-CPU held-stack overflow (%d)", c.depth);
    }
    c.held[c.depth++] = lock;
}

void lockdep_releasing(const void* lock) {
    uint32_t cpu = percpu()->cpu_id;
    if (cpu >= kMaxCpus) {
        return;
    }
    HeldStack& c = g_held[cpu];
    // Fast path: LIFO release (the overwhelmingly common case).
    if (c.depth > 0 && c.held[c.depth - 1] == lock) {
        c.depth--;
        return;
    }
    // Out-of-order release: find + compact.  Reported -- nested guards are LIFO,
    // so this indicates an unbalanced acquire/release pairing.
    for (int i = 0; i < c.depth; i++) {
        if (c.held[i] == lock) {
            for (int j = i; j < c.depth - 1; j++) {
                c.held[j] = c.held[j + 1];
            }
            c.depth--;
            cinux::lib::kprintf("lockdep: out-of-order spinlock release %p\n", lock);
            return;
        }
    }
    cinux::lib::kprintf("lockdep: release of spinlock %p not held on this CPU\n", lock);
}

uint32_t lockdep_held_depth() {
    uint32_t cpu = percpu()->cpu_id;
    return (cpu < kMaxCpus) ? static_cast<uint32_t>(g_held[cpu].depth) : 0;
}

}  // namespace cinux::proc

#endif  // CINUX_LOCKDEP
