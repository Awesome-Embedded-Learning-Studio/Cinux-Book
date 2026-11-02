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

#include <atomic>
#include <limits>
#include <type_traits>

#include "cinux/assert.hpp"

namespace cinux::base {

/**
 * @brief Ownership counter starting with one reference; last release reports true.
 * @tparam Count Unsigned integral storage type, excluding bool.
 * @note Atomic ownership changes publish with acquire/release ordering.
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
        const Count kBefore = count_.fetch_add(1, std::memory_order_acq_rel);
        safety::Check(kBefore != 0, "reference acquired without a live owner");
        safety::Check(kBefore != std::numeric_limits<Count>::max(), "reference count overflow");
    }

    /**
     * @brief Acquire an owner only while at least one owner remains.
     * @return False after the final release; zero is never resurrected.
     * @note The caller must independently prevent storage reclamation during this call.
     */
    [[nodiscard]] bool try_retain() {
        Count observed = count_.load(std::memory_order_acquire);
        while (observed != 0) {
            constexpr auto kCeiling = std::numeric_limits<Count>::max();
            safety::Check(observed != kCeiling, "reference count overflow");
            if (count_.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Drop one owner; releasing an empty counter terminates.
     * @return True exactly when the last owning reference is released.
     * @since 0.1.0
     * @ingroup base_ptr
     */
    [[nodiscard]] bool release() {
        const Count kBefore = count_.fetch_sub(1, std::memory_order_acq_rel);
        safety::Check(kBefore != 0, "reference released without an owner");
        return kBefore == 1;
    }

    /// @brief Number of owning references, read under the caller's synchronization.
    [[nodiscard]] Count use_count() const { return count_.load(std::memory_order_acquire); }

private:
    std::atomic<Count> count_{1};
};

}  // namespace cinux::base
