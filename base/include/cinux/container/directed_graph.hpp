/**
 * @file    directed_graph.hpp
 * @brief   Allocation-free directed graph over a bounded set of indices.
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_container
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <bit>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::base {

/**
 * @brief Directed adjacency graph with at most one machine word of vertices.
 * @tparam Capacity Number of vertices, between one and 64 inclusive.
 * @note All vertex arguments must be below Capacity. Identity and lifetime
 *       belong to the caller. Traversal uses bounded local masks, no recursion.
 * @since 0.1.0
 * @ingroup base_container
 */
template <unsigned int Capacity>
class DirectedGraph {
    static_assert(Capacity > 0 && Capacity <= 64);
    using Mask = bit::BitMask<unsigned long long>;

public:
    /// @brief Add a directed edge; duplicate edges are harmless.
    constexpr void connect(unsigned int source, unsigned int target) {
        edges_[source] =
            edges_[source] | bit::MaskBit<unsigned long long>(static_cast<unsigned char>(target));
    }

    /// @brief Whether target is reachable from source, including a zero-length path.
    [[nodiscard]] constexpr bool reaches(unsigned int source, unsigned int target) const {
        Mask pending = bit::MaskBit<unsigned long long>(static_cast<unsigned char>(source));
        Mask seen;
        while (pending.raw != 0) {
            const auto kSlot = static_cast<unsigned int>(std::countr_zero(pending.raw));
            if (kSlot == target) {
                return true;
            }
            const auto kBit = bit::MaskBit<unsigned long long>(static_cast<unsigned char>(kSlot));
            seen            = seen | kBit;
            pending         = (pending & ~kBit) | (edges_[kSlot] & ~seen);
        }
        return false;
    }

    /// @brief Remove every edge incident to a vertex before its index is reused.
    constexpr void erase(unsigned int vertex) {
        edges_[vertex]   = Mask{};
        const auto kKeep = ~bit::MaskBit<unsigned long long>(static_cast<unsigned char>(vertex));
        for (auto& edge : edges_) {
            edge = edge & kKeep;
        }
    }

private:
    Mask edges_[Capacity]{};
};

}  // namespace cinux::base
