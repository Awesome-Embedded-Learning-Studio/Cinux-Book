/**
 * @file    lockdep.hpp
 * @brief   Shared lock-order graph with per-CPU held chains.
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "cinux/container/directed_graph.hpp"
#include "cinux/container/slot_table.hpp"
#include "cinux/container/static_stack.hpp"
#include "kernel/arch/x86_64/per_cpu_config.hpp"

namespace cinux::proc {

/// One line of the held-chain dump: the lock's registered label, or
/// null when the lock has no name and only the address speaks.
using LockdepSink = void (*)(const char* label, unsigned long long address);

/**
 * @brief   Install where the held-chain dump lines go.
 * @note    Kernel-only callers hand in a printing bridge; without one
 *          the dump is a silent walk, so host tests and dump-free
 *          builds lose nothing.
 * @since   0.4.0
 * @ingroup kernel_proc
 */
void InstallLockdepSink(LockdepSink sink);

/**
 * @brief Instance-based lock-order checker; callers serialize access.
 * @note  Identity is a live lock address. forget() removes destroyed locks.
 *        Capacity exhaustion fails instead of silently losing coverage.
 * @since 0.1.0
 * @ingroup kernel_proc
 */
class LockOrder {
public:
    /// Result of an acquisition or release validation.
    enum class Status : unsigned char {
        kOk,
        kCycle,
        kRecursive,
        kCapacity,
        kNotHeld,
        kOutOfOrder,
    };
    /// Maximum distinct live locks and maximum simultaneous nesting.
    static constexpr unsigned int kLocks = 64;
    static constexpr unsigned int kDepth = 16;

    /// Record an acquisition only when it preserves an acyclic graph.
    [[nodiscard]] Status acquire(const void* lock);

    /// Release only the innermost held lock, preserving IRQ restore order.
    [[nodiscard]] Status release(const void* lock);

    /// Remove a destroyed, unheld lock and every incident edge.
    [[nodiscard]] bool forget(const void* lock);

    /// Register the human name of a lock ahead of its first acquisition.
    void name_lock(const void* lock, const char* label);

    /**
     * @brief   Walk the held chain, one sink call per lock, innermost last.
     * @since   0.4.0
     */
    void dump_held();

private:
    using HeldChain = base::container::StaticStack<unsigned int, kDepth>;
    HeldChain&                 held();
    [[nodiscard]] unsigned int find_lock(const void* lock) const;
    [[nodiscard]] unsigned int vacant_slot() const;

    base::container::SlotTable<const void*, kLocks> locks_;
    const char*                                     names_[kLocks] = {};
    base::DirectedGraph<kLocks>                     graph_;
    HeldChain                                       held_[arch::per_cpu::kMaxCpus];
};

}  // namespace cinux::proc
