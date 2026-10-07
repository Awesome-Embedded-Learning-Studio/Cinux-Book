/**
 * @file    static_stack.hpp
 * @brief   A fixed-capacity stack with inline, constant-initializable storage.
 * @author  Charliechen114514
 * @date    2026-10-08
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_container
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <array>
#include <span>
#include <utility>

namespace cinux::base::container {

/**
 * @brief A bounded LIFO container without allocation or concurrency policy.
 * @tparam Element Default-initializable and move-assignable stored type.
 * @tparam Capacity Maximum number of elements, including zero.
 * @note Storage elements are initialized with the stack. pop resets the
 *       removed element; failed push/pop leave resident elements unchanged.
 * @since 0.1.0
 * @ingroup base_container
 */
template <typename Element, unsigned int Capacity>
class StaticStack {
public:
    /// @brief Number of resident elements.
    [[nodiscard]] constexpr unsigned int size() const { return size_; }

    /// @brief Whether no elements are resident.
    [[nodiscard]] constexpr bool empty() const { return size_ == 0; }

    /// @brief Whether the inline storage has no free slots.
    [[nodiscard]] constexpr bool full() const { return size_ == Capacity; }

    /// @brief Push an element; return false when full.
    [[nodiscard]] constexpr bool push(Element value) {
        if (full()) {
            return false;
        }
        elements_[size_] = std::move(value);
        ++size_;
        return true;
    }

    /// @brief Remove the top element; return false when empty.
    [[nodiscard]] constexpr bool pop() {
        if (empty()) {
            return false;
        }
        elements_[size_ - 1] = Element{};
        --size_;
        return true;
    }

    /**
     * @brief Move the top element into output, or return false when empty.
     * @param[out] output Destination outside this stack's storage.
     * @return Whether an element was removed; false leaves output unchanged.
     * @since 0.1.0
     * @ingroup base_container
     */
    [[nodiscard]] constexpr bool pop(Element& output) {
        if (empty()) {
            return false;
        }
        output = std::move(elements_[size_ - 1]);
        return pop();
    }

    /// @brief Pointer to the top element, or nullptr when empty.
    [[nodiscard]] constexpr Element* top() { return empty() ? nullptr : &elements_[size_ - 1]; }

    /// @brief Const pointer to the top element, or nullptr when empty.
    [[nodiscard]] constexpr const Element* top() const {
        return empty() ? nullptr : &elements_[size_ - 1];
    }

    /// @brief Borrowed resident sequence in bottom-to-top order.
    [[nodiscard]] constexpr std::span<const Element> view() const {
        return {elements_.data(), size_};
    }

private:
    std::array<Element, Capacity> elements_{};
    unsigned int                  size_ = 0;
};

}  // namespace cinux::base::container
