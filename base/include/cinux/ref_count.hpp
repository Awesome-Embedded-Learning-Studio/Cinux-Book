/**
 * @file    ref_count.hpp
 * @brief   Checked intrusive ownership counting without a destruction policy.
 * @author  Charliechen114514
 * @date    2026-10-08
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_ptr
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <limits>
#include <type_traits>

#include "cinux/assert.hpp"

namespace cinux::base {

/**
 * @brief Ownership counter starting with one reference; last release reports true.
 * @tparam Count Unsigned integral storage type, excluding bool.
 * @note Callers serialize access and destroy their object at the last release.
 *       Borrowed pointers add no reference. Zero cannot be retained again.
 *       Copying and moving are forbidden because ownership belongs to one object.
 * @since 0.1.0
 * @ingroup base_ptr
 */
template <typename Count = unsigned long>
class RefCount {
    static_assert(std::is_integral_v<Count> && std::is_unsigned_v<Count> &&
                  !std::is_same_v<Count, bool>);

public:
    constexpr RefCount()                 = default;
    RefCount(const RefCount&)            = delete;
    RefCount& operator=(const RefCount&) = delete;
    RefCount(RefCount&&)                 = delete;
    RefCount& operator=(RefCount&&)      = delete;

    /**
     * @brief Add one owning reference; invalid acquisition or overflow terminates.
     * @return None.
     * @since 0.1.0
     * @ingroup base_ptr
     */
    void retain() {
        safety::Check(count_ != 0, "reference acquired without a live owner");
        safety::Check(count_ != std::numeric_limits<Count>::max(), "reference count overflow");
        ++count_;
    }

    /**
     * @brief Drop one owner; releasing an empty counter terminates.
     * @return True exactly when the last owning reference is released.
     * @since 0.1.0
     * @ingroup base_ptr
     */
    [[nodiscard]] bool release() {
        safety::Check(count_ != 0, "reference released without an owner");
        --count_;
        return count_ == 0;
    }

    /// @brief Number of owning references, read under the caller's synchronization.
    [[nodiscard]] Count use_count() const { return count_; }

private:
    Count count_ = 1;
};

}  // namespace cinux::base
