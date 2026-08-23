/**
 * @file optional.hpp
 * @brief Optional value container — "found/not-found" semantics without Error codes.
 */

#ifndef CINUX_OPTIONAL_HPP
#define CINUX_OPTIONAL_HPP

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>

namespace cinux::lib {

// ============================================================
// Nullopt sentinel
// ============================================================

struct NulloptT {
    explicit constexpr NulloptT(int) {}
};

inline constexpr NulloptT nullopt{0};

// ============================================================
// Optional<T>
// ============================================================

/**
 * @brief Container that may or may not hold a value.
 *
 * @tparam T  Value type.
 *
 * Lighter than ErrorOr for "found/not-found" use cases.
 */
template <typename T>
class Optional {
    union Storage {
        T value_;
        constexpr Storage() {}
        ~Storage() {}
    };

public:
    constexpr Optional() : has_value_(false) {}
    constexpr Optional(NulloptT) : has_value_(false) {}  // NOLINT

    Optional(const T& value) : has_value_(true) {  // NOLINT
        new (&storage_.value_) T(value);
    }

    Optional(T&& value) : has_value_(true) {  // NOLINT
        new (&storage_.value_) T(std::move(value));
    }

    Optional(const Optional& other) : has_value_(other.has_value_) {
        if (has_value_) {
            new (&storage_.value_) T(other.storage_.value_);
        }
    }

    Optional(Optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
        : has_value_(other.has_value_) {
        if (has_value_) {
            new (&storage_.value_) T(std::move(other.storage_.value_));
        }
    }

    Optional& operator=(const Optional& other) {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            if (has_value_) {
                new (&storage_.value_) T(other.storage_.value_);
            }
        }
        return *this;
    }

    Optional& operator=(Optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            if (has_value_) {
                new (&storage_.value_) T(std::move(other.storage_.value_));
            }
        }
        return *this;
    }

    Optional& operator=(NulloptT) {
        destroy();
        has_value_ = false;
        return *this;
    }

    ~Optional() { destroy(); }

    constexpr bool     has_value() const { return has_value_; }
    constexpr explicit operator bool() const { return has_value_; }

    T& value() {
        assert(has_value_ && "Optional::value() called on empty");
        return storage_.value_;
    }

    const T& value() const {
        assert(has_value_ && "Optional::value() called on empty");
        return storage_.value_;
    }

    T&       operator*() { return value(); }
    const T& operator*() const { return value(); }
    T*       operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    constexpr T value_or(const T& default_val) const {
        return has_value_ ? storage_.value_ : default_val;
    }

    void reset() {
        destroy();
        has_value_ = false;
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        destroy();
        new (&storage_.value_) T(std::forward<Args>(args)...);
        has_value_ = true;
        return storage_.value_;
    }

private:
    void destroy() {
        if (has_value_) {
            storage_.value_.~T();
        }
    }

    Storage storage_;
    bool    has_value_;
};

}  // namespace cinux::lib

#endif  // CINUX_OPTIONAL_HPP
