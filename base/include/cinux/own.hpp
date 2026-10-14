/**
 * @file    own.hpp
 * @brief   Own: the one pointer that deletes what it holds.
 *
 * Every raw new in this tree once demanded a matching delete on every
 * path out — each early return a chance to leak, each rollback chain a
 * place to forget one. Own closes that whole class: it deletes its
 * object when it goes out of scope, cannot be copied, and hands its
 * hold off only by explicit move or release. Borrowing stays raw
 * pointers, on purpose: a borrow that outlives its owner is a design
 * bug no wrapper can fix, and the code reads clearer when ownership is
 * spelled and borrows are not.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_memory
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>
#include <utility>

namespace cinux::base {

/**
 * @brief         The sole owner of one heap object.
 * @tparam        Stuff   The owned type.
 * @note          Move-only; destruction deletes. Default construction
 *                holds nothing, and a moved-from Own holds nothing.
 * @since         0.1.0
 * @ingroup       base_memory
 */
template <typename Stuff>
class Own {
public:
    /// Holds nothing.
    Own() = default;

    /// Takes hold of one live object.
    explicit Own(Stuff* held) : held_(held) {}

    /// Copies are refused: exactly one owner.
    Own(const Own&)            = delete;
    Own& operator=(const Own&) = delete;

    /// Moves carry the hold; the source lets go.
    Own(Own&& other) noexcept : held_(other.held_) { other.held_ = nullptr; }

    Own& operator=(Own&& other) noexcept {
        if (this != &other) {
            delete held_;
            held_       = other.held_;
            other.held_ = nullptr;
        }
        return *this;
    }

    /// Deletes whatever is held.
    ~Own() { delete held_; }

    /// Whether anything is held.
    [[nodiscard]] bool holds() const { return held_ != nullptr; }

    /// The borrowed object; null when holding nothing.
    [[nodiscard]] Stuff* get() const { return held_; }

    /// Hands the hold away permanently.
    [[nodiscard]] Stuff* release() {
        Stuff* const kOut = held_;
        held_             = nullptr;
        return kOut;
    }

    /// Deletes the current hold and takes a new one.
    void reset(Stuff* fresh = nullptr) {
        delete held_;
        held_ = fresh;
    }

    /// Reaches the owned object.
    Stuff& operator*() const { return *held_; }

    /// Reaches the owned object.
    Stuff* operator->() const { return held_; }

private:
    Stuff* held_ = nullptr;
};

/**
 * @brief         The sole owner of one heap array.
 * @tparam        Stuff   The element type.
 * @note          Same discipline as the single-object Own, deleting
 *                with [] and indexing like the array it owns.
 * @since         0.1.0
 * @ingroup       base_memory
 */
template <typename Stuff>
class Own<Stuff[]> {
public:
    /// Holds nothing.
    Own() = default;

    /// Takes hold of one live array.
    explicit Own(Stuff* held) : held_(held) {}

    /// Copies are refused: exactly one owner.
    Own(const Own&)            = delete;
    Own& operator=(const Own&) = delete;

    /// Moves carry the hold; the source lets go.
    Own(Own&& other) noexcept : held_(other.held_) { other.held_ = nullptr; }

    Own& operator=(Own&& other) noexcept {
        if (this != &other) {
            delete[] held_;
            held_       = other.held_;
            other.held_ = nullptr;
        }
        return *this;
    }

    /// Deletes whatever is held.
    ~Own() { delete[] held_; }

    /// Whether anything is held.
    [[nodiscard]] bool holds() const { return held_ != nullptr; }

    /// The borrowed array; null when holding nothing.
    [[nodiscard]] Stuff* get() const { return held_; }

    /// Hands the hold away permanently.
    [[nodiscard]] Stuff* release() {
        Stuff* const kOut = held_;
        held_             = nullptr;
        return kOut;
    }

    /// Deletes the current hold and takes a new one.
    void reset(Stuff* fresh = nullptr) {
        delete[] held_;
        held_ = fresh;
    }

    /// Indexes the owned array.
    Stuff& operator[](std::size_t index) const { return held_[index]; }

private:
    Stuff* held_ = nullptr;
};

/**
 * @brief         Makes one object and hands back its sole owner.
 * @tparam        Stuff   The type to build.
 * @tparam        Args    Constructor argument types.
 * @param[in]     args    Constructor arguments.
 * @return        An Own holding the new object; on a kernel heap
 *                whose new terminates on exhaustion the empty return
 *                never happens, and host builds keep it as the cheap
 *                guard it is.
 * @since         0.1.0
 * @ingroup       base_memory
 */
template <typename Stuff, typename... Args>
[[nodiscard]] Own<Stuff> MakeOwn(Args&&... args) {
    return Own<Stuff>(new Stuff(std::forward<Args>(args)...));
}

/**
 * @brief         Makes one default-initialized array and hands back its owner.
 * @tparam        Stuff   The element type.
 * @param[in]     count   Element count.
 * @return        An Own holding the new array; exhaustion behaves
 *                as the single-object MakeOwn documents.
 * @since         0.1.0
 * @ingroup       base_memory
 */
template <typename Stuff>
[[nodiscard]] Own<Stuff[]> MakeOwnArray(std::size_t count) {
    return Own<Stuff[]>(new Stuff[count]{});
}

}  // namespace cinux::base
