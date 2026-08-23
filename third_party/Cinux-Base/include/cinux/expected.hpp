/**
 * @file expected.hpp
 * @brief Value/Error discriminated union — ErrorOr<T> and ErrorOr<void>.
 *
 * Replaces bare int error codes and errno with a type-safe alternative.
 * No exceptions used; value() on error path calls assert.
 */

#ifndef CINUX_EXPECTED_HPP
#define CINUX_EXPECTED_HPP

#include <cassert>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace cinux::lib {

// ============================================================
// Error enum
// ============================================================

/** @brief Common error codes for CinuxOS subsystems. */
enum class Error : uint32_t {
    Ok = 0,
    OutOfMemory,
    InvalidArgument,
    NotFound,
    IOError,
    AlreadyExists,
    PermissionDenied,
    WouldBlock,
    BufferOverflow,
    NotImplemented,
    BrokenPipe,
    ConnectionRefused,
    TimedOut,
    Busy,
    Fault,  ///< Bad address (EFAULT): user pointer rejected by access_ok or copy fault
    NotADirectory,  ///< ENOTDIR: a component used as a directory is not one
    Loop,           ///< ELOOP: too many symbolic links during path resolution
};

/** @brief Convert an Error code to a human-readable string. */
constexpr const char* error_string(Error e) {
    switch (e) {
    case Error::Ok:
        return "Ok";
    case Error::OutOfMemory:
        return "OutOfMemory";
    case Error::InvalidArgument:
        return "InvalidArgument";
    case Error::NotFound:
        return "NotFound";
    case Error::IOError:
        return "IOError";
    case Error::AlreadyExists:
        return "AlreadyExists";
    case Error::PermissionDenied:
        return "PermissionDenied";
    case Error::WouldBlock:
        return "WouldBlock";
    case Error::BufferOverflow:
        return "BufferOverflow";
    case Error::NotImplemented:
        return "NotImplemented";
    case Error::BrokenPipe:
        return "BrokenPipe";
    case Error::ConnectionRefused:
        return "ConnectionRefused";
    case Error::TimedOut:
        return "TimedOut";
    case Error::Busy:
        return "Busy";
    case Error::Fault:
        return "Fault";
    case Error::NotADirectory:
        return "NotADirectory";
    case Error::Loop:
        return "Loop";
    }
    return "Unknown";
}

// ============================================================
// ErrorOr<T>
// ============================================================

/**
 * @brief Discriminated union holding either a value of type T or an Error.
 *
 * @tparam T  Value type.
 *
 * Usage:
 * @code
 *   ErrorOr<int> result = some_fn();
 *   if (result.ok()) {
 *       use(result.value());
 *   } else {
 *       handle(result.error());
 *   }
 * @endcode
 */
template <typename T>
class [[nodiscard]] ErrorOr {
    // Union with trivial ctor/dtor — lifetime managed by ErrorOr.
    union Storage {
        T     value_;
        Error error_;
        constexpr Storage() : error_{} {}
        ~Storage() {}
    };

public:
    /** @brief Success path — construct with a value. */
    ErrorOr(T value) : is_ok_(true) {  // NOLINT
        new (&storage_.value_) T(std::move(value));
    }

    /** @brief Error path — construct with an Error code. */
    constexpr ErrorOr(Error err) : is_ok_(false) {  // NOLINT
        storage_.error_ = err;
    }

    /** @brief Copy constructor. */
    ErrorOr(const ErrorOr& other) : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&storage_.value_) T(other.storage_.value_);
        } else {
            storage_.error_ = other.storage_.error_;
        }
    }

    /** @brief Move constructor. */
    ErrorOr(ErrorOr&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
        : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&storage_.value_) T(std::move(other.storage_.value_));
        } else {
            storage_.error_ = other.storage_.error_;
        }
    }

    /** @brief Copy assignment. */
    ErrorOr& operator=(const ErrorOr& other) {
        if (this != &other) {
            destroy();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                new (&storage_.value_) T(other.storage_.value_);
            } else {
                storage_.error_ = other.storage_.error_;
            }
        }
        return *this;
    }

    /** @brief Move assignment. */
    ErrorOr& operator=(ErrorOr&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (this != &other) {
            destroy();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                new (&storage_.value_) T(std::move(other.storage_.value_));
            } else {
                storage_.error_ = other.storage_.error_;
            }
        }
        return *this;
    }

    /** @brief Destructor — destroys the active union member. */
    ~ErrorOr() { destroy(); }

    /** @brief True if this holds a value. */
    constexpr bool ok() const { return is_ok_; }

    /** @brief Implicit bool conversion — true if ok. */
    constexpr explicit operator bool() const { return is_ok_; }

    /** @brief Get the stored value. Asserts if !ok(). */
    T& value() {
        assert(is_ok_ && "ErrorOr::value() called on error");
        return storage_.value_;
    }

    /** @overload value() const. */
    const T& value() const {
        assert(is_ok_ && "ErrorOr::value() called on error");
        return storage_.value_;
    }

    T&       operator*() { return value(); }
    const T& operator*() const { return value(); }
    T*       operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    /** @brief Get the error code. Only valid when !ok(). */
    constexpr Error error() const { return storage_.error_; }

private:
    void destroy() {
        if (is_ok_) {
            storage_.value_.~T();
        }
    }

    Storage storage_;
    bool    is_ok_;
};

// ============================================================
// ErrorOr<void> specialization
// ============================================================

/** @brief Specialization for operations that can fail but return no value. */
template <>
class [[nodiscard]] ErrorOr<void> {
public:
    /** @brief Success. */
    constexpr ErrorOr() : error_(Error::Ok), is_ok_(true) {}

    /** @brief Error path. */
    constexpr ErrorOr(Error err) : error_(err), is_ok_(false) {}  // NOLINT

    constexpr ErrorOr(const ErrorOr&)            = default;
    constexpr ErrorOr(ErrorOr&&)                 = default;
    constexpr ErrorOr& operator=(const ErrorOr&) = default;
    constexpr ErrorOr& operator=(ErrorOr&&)      = default;

    constexpr bool     ok() const { return is_ok_; }
    constexpr explicit operator bool() const { return is_ok_; }
    constexpr Error    error() const { return error_; }

private:
    Error error_;
    bool  is_ok_;
};

}  // namespace cinux::lib

#endif  // CINUX_EXPECTED_HPP
