/**
 * @file    slot_table.hpp
 * @brief   Inline occupied slots with stable indices and lowest-first reuse.
 * @author  Charliechen114514
 * @date    2026-10-08
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_container
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <array>
#include <optional>
#include <utility>

namespace cinux::base::container {

/**
 * @brief Fixed-capacity table whose occupied values never change index.
 * @tparam Element Stored type; occupancy is independent of its value.
 * @tparam Capacity Number of slots; Capacity is the invalid-index sentinel.
 * @note No allocation or concurrency policy. Erasing ends the stored object's
 *       lifetime, invalidating its pointer; other slots remain untouched.
 * @since 0.1.0
 * @ingroup base_container
 */
template <typename Element, unsigned long Capacity>
class SlotTable {
public:
    /// @brief First empty slot at or above first, or Capacity when none exists.
    [[nodiscard]] constexpr unsigned long vacant(unsigned long first = 0) const {
        for (unsigned long index = first; index < Capacity; ++index) {
            if (!slots_[index].has_value()) {
                return index;
            }
        }
        return Capacity;
    }

    /// @brief Store in the first empty slot at or above first; return its index or Capacity.
    [[nodiscard]] constexpr unsigned long insert(Element value, unsigned long first = 0) {
        const auto kIndex = vacant(first);
        if (kIndex != Capacity) {
            slots_[kIndex].emplace(std::move(value));
        }
        return kIndex;
    }

    /// @brief Default-construct in the first empty slot at or above first, for elements
    ///        that cannot move; return its index or Capacity.
    /// @note The caller fills the object through get() afterwards, still holding whatever
    ///       lock guards this table.
    [[nodiscard]] constexpr unsigned long insert_default(unsigned long first = 0) {
        const auto kIndex = vacant(first);
        if (kIndex != Capacity) {
            slots_[kIndex].emplace();
        }
        return kIndex;
    }

    /// @brief Stored object at index, or nullptr for an empty or invalid slot.
    [[nodiscard]] constexpr Element* get(unsigned long index) {
        if (index >= Capacity) {
            return nullptr;
        }
        auto& slot = slots_[index];
        return slot.has_value() ? &*slot : nullptr;
    }

    /// @brief Const stored object at index, or nullptr for an empty or invalid slot.
    [[nodiscard]] constexpr const Element* get(unsigned long index) const {
        if (index >= Capacity) {
            return nullptr;
        }
        const auto& slot = slots_[index];
        return slot.has_value() ? &*slot : nullptr;
    }

    /// @brief End one occupied value's lifetime; return false for an empty or invalid slot.
    [[nodiscard]] constexpr bool erase(unsigned long index) {
        if (get(index) == nullptr) {
            return false;
        }
        slots_[index].reset();
        return true;
    }

    /// @brief Find the first occupied value matching predicate, or return Capacity.
    template <typename Predicate>
    [[nodiscard]] constexpr unsigned long find_if(Predicate predicate) const {
        for (unsigned long index = 0; index < Capacity; ++index) {
            const auto& slot = slots_[index];
            if (slot.has_value() && predicate(*slot)) {
                return index;
            }
        }
        return Capacity;
    }

    /// @brief Visit occupied (index, value) pairs in index order; erasing the current slot is
    /// valid.
    template <typename Visitor>
    constexpr void for_each(Visitor visitor) {
        for (unsigned long index = 0; index < Capacity; ++index) {
            auto& slot = slots_[index];
            if (slot.has_value()) {
                visitor(index, *slot);
            }
        }
    }

    /// @brief Visit const occupied (index, value) pairs in index order.
    template <typename Visitor>
    constexpr void for_each(Visitor visitor) const {
        for (unsigned long index = 0; index < Capacity; ++index) {
            const auto& slot = slots_[index];
            if (slot.has_value()) {
                visitor(index, *slot);
            }
        }
    }

private:
    std::array<std::optional<Element>, Capacity> slots_{};
};

}  // namespace cinux::base::container
