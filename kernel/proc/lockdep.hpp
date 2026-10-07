/**
 * @file    lockdep.hpp
 * @brief   Bounded lock-order graph for one interrupt-excluded core.
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

namespace cinux::proc {

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
        kOutOfOrder
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

private:
    [[nodiscard]] unsigned int find_lock(const void* lock) const;
    [[nodiscard]] unsigned int vacant_slot() const;

    base::container::SlotTable<const void*, kLocks>    locks_;
    base::DirectedGraph<kLocks>                        graph_;
    base::container::StaticStack<unsigned int, kDepth> held_;
};

}  // namespace cinux::proc
