/**
 * @file    result.hpp
 * @brief   Value/Error discriminated union — Result<T> and Result<void>.
 *
 * Replaces bare int error codes and errno with a type-safe alternative:
 * no exceptions are used, and value() on the error path terminates through
 * the assertion hook. The error universe is shared with the whole base
 * layer (KernelError) and stays deliberately small — entries are added
 * when a real caller hits a missing case, not up front.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_result
 */

#pragma once

#include <cinux/assert.hpp>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace cinux::base {

/**
 * @brief   Common error codes shared by the base layer.
 *
 * Six entries on day one; kCount is a sentinel used by ErrorString() to
 * keep its name table in sync at compile time.
 */
enum class KernelError : std::uint8_t {
    kOk = 0,           ///< No error.
    kOutOfMemory,      ///< Allocation failed.
    kInvalidArgument,  ///< Caller passed an unusable argument.
    kNotFound,         ///< Looked-up object does not exist.
    kIOError,          ///< Device or transport failure.
    kWouldBlock,       ///< Operation would block in non-blocking mode.

    kCount,  ///< Sentinel: number of real entries. Never returned.
};

/**
 * @brief         Maps an error code to its name.
 *
 * @param[in]     error   Error code to name.
 * @return        NUL-terminated name of the entry, "Unknown" for values
 *                outside the enum range.
 * @note          Table lookup by underlying value; a static_assert pins the
 *                table length to kCount so adding an enum entry without a
 *                table row fails to compile.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_result
 */
constexpr const char* ErrorString(KernelError error) {
    constexpr const char* kNames[] = {
        "Ok", "OutOfMemory", "InvalidArgument", "NotFound", "IOError", "WouldBlock",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == std::to_underlying(KernelError::kCount));

    const auto kIndex = std::to_underlying(error);
    return kIndex < std::to_underlying(KernelError::kCount) ? kNames[kIndex] : "Unknown";
}

/**
 * @brief   Discriminated union holding either a value or an error.
 *
 * Exactly one of the two is alive at any moment; the union plus placement
 * new keeps the layout compact in allocation-free environments. Ignoring a
 * returned Result is a compile error ([[nodiscard]]).
 *
 * @tparam  RawResult   Value type stored on success.
 * @tparam  ErrorType   Error enum; defaults to KernelError.
 */
template <typename RawResult, typename ErrorType = KernelError>
class [[nodiscard("Dont throw away the Result, check it!")]] Result {
public:
    /**
     * @brief         Success path — construct with a value.
     *
     * @param[in]     value   Value to store.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    Result(RawResult value) : is_ok_(true) {  // NOLINT(google-explicit-constructor)
        new (&internal_storage_.value) RawResult(std::move(value));
    }

    /**
     * @brief         Error path — construct with an error code.
     *
     * @param[in]     error   Error code to store.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    constexpr Result(ErrorType error) : is_ok_(false) {  // NOLINT(google-explicit-constructor)
        internal_storage_.error = error;
    }

    /**
     * @brief         Copy constructor.
     *
     * @param[in]     other   Source to copy from.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    Result(const Result& other) : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&internal_storage_.value) RawResult(other.internal_storage_.value);
        } else {
            internal_storage_.error = other.internal_storage_.error;
        }
    }

    /**
     * @brief         Move constructor.
     *
     * @param[in]     other   Source to move from.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<RawResult>)
        : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&internal_storage_.value) RawResult(std::move(other.internal_storage_.value));
        } else {
            internal_storage_.error = other.internal_storage_.error;
        }
    }

    /**
     * @brief         Copy assignment — destroy-then-construct.
     *
     * @param[in]     other   Source to copy from.
     * @return        Reference to this object.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    Result& operator=(const Result& other) {
        if (this != &other) {
            release_self();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                new (&internal_storage_.value) RawResult(other.internal_storage_.value);
            } else {
                internal_storage_.error = other.internal_storage_.error;
            }
        }
        return *this;
    }

    /**
     * @brief         Move assignment — destroy-then-construct.
     *
     * @param[in]     other   Source to move from.
     * @return        Reference to this object.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    Result& operator=(Result&& other) noexcept(std::is_nothrow_move_constructible_v<RawResult>) {
        if (this != &other) {
            release_self();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                new (&internal_storage_.value) RawResult(std::move(other.internal_storage_.value));
            } else {
                internal_storage_.error = other.internal_storage_.error;
            }
        }
        return *this;
    }

    /**
     * @brief         Destructor — destroys whichever member is active.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    ~Result() { release_self(); }

    /**
     * @brief         Reports whether this object holds a value.
     *
     * @return        true when a value is stored.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    [[nodiscard]] constexpr bool ok() const { return is_ok_; }

    /**
     * @brief         Implicit bool conversion — true when ok().
     *
     * @return        true when a value is stored.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    constexpr explicit operator bool() const { return is_ok_; }

    /**
     * @brief         Accesses the stored value.
     *
     * @return        Reference to the stored value.
     * @note          Terminates through the assertion hook when !ok():
     *                taking a value without checking fails loudly instead
     *                of propagating garbage deeper into the call chain.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    RawResult& value() {
        safety::Check(is_ok_, "Result::value() called on error");
        return internal_storage_.value;
    }

    /**
     * @brief         Accesses the stored value (const overload).
     *
     * @return        Const reference to the stored value.
     * @note          Terminates through the assertion hook when !ok().
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    [[nodiscard]] const RawResult& value() const {
        safety::Check(is_ok_, "Result::value() called on error");
        return internal_storage_.value;
    }

    /**
     * @brief         Dereferences to the stored value.
     *
     * @return        Reference to the stored value.
     * @note          Same failure semantics as value().
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    RawResult& operator*() { return value(); }

    /**
     * @brief         Dereferences to the stored value (const overload).
     *
     * @return        Const reference to the stored value.
     * @note          Same failure semantics as value().
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    const RawResult& operator*() const { return value(); }

    /**
     * @brief         Member access into the stored value.
     *
     * @return        Pointer to the stored value.
     * @note          Same failure semantics as value().
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    RawResult* operator->() { return &value(); }

    /**
     * @brief         Member access into the stored value (const overload).
     *
     * @return        Const pointer to the stored value.
     * @note          Same failure semantics as value().
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    const RawResult* operator->() const { return &value(); }

    /**
     * @brief         Reads the stored error code.
     *
     * @return        The error stored on the error path; kOk on the
     *                success path.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    [[nodiscard]] constexpr ErrorType error() const { return internal_storage_.error; }

private:
    /**
     * @brief         Destroys the active union member, if it is a value.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    void release_self() {
        if (!is_ok_) {
            return;
        }
        internal_storage_.value.~RawResult();
    }

    union Storage {
        RawResult value;
        ErrorType error;
        constexpr Storage() : error{} {}
        ~Storage() {}
    };

    Storage internal_storage_;
    bool    is_ok_;
};

/**
 * @brief   Specialization for operations that can fail but return nothing.
 *
 * @tparam  ErrorType   Error enum; defaults to KernelError.
 */
template <typename ErrorType>
class [[nodiscard("Dont throw away the Result, check it!")]] Result<void, ErrorType> {
public:
    /**
     * @brief         Success path.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    constexpr Result() : error_(ErrorType::kOk), is_ok_(true) {}

    /**
     * @brief         Error path — construct with an error code.
     *
     * @param[in]     error   Error code to store.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr Result(ErrorType error) : error_(error), is_ok_(false) {}

    /**
     * @brief         Reports whether this object represents success.
     *
     * @return        true on success.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    [[nodiscard]] constexpr bool ok() const { return is_ok_; }

    /**
     * @brief         Implicit bool conversion — true on success.
     *
     * @return        true on success.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    constexpr explicit operator bool() const { return is_ok_; }

    /**
     * @brief         Reads the stored error code.
     *
     * @return        The stored error, kOk on success.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_result
     */
    [[nodiscard]] constexpr ErrorType error() const { return error_; }

private:
    ErrorType error_;
    bool      is_ok_;
};

}  // namespace cinux::base
