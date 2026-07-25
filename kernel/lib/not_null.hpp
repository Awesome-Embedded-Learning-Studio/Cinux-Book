/**
 * @file kernel/lib/not_null.hpp
 * @brief NotNull<T> -- a non-null pointer contract wrapper (F-INFRA I-7)
 *
 * A zero-overhead marker (one pointer, no extra state) that encodes "this
 * pointer is never null" in the type system and traps at construction (via
 * assert -> __assert_fail -> kpanic) if a nullptr reaches it. Modelled on
 * gsl::not_null, stripped of <memory>/smart-pointer support to stay
 * freestanding. Lives in kernel/lib (cinux::lib), like kprintf/klog; it can be
 * promoted to Cinux-Base later if host tests need it.
 *
 * Two-way implicit conversion makes it a near drop-in: callers pass a raw T
 * unchanged (implicit ctor asserts non-null), and a NotNull converts back to T
 * (implicit operator T) so existing bodies that take T keep compiling.
 *
 * HONEST LIMITATION: NotNull enforces non-nullness only. It CANNOT statically
 * prevent use-after-free / dangling -- pair it with the Clang static analyzer
 * (F-INFRA I-8) and the slab redzone allocator (R10) for that. It is a contract
 * marker, not a borrow checker.
 *
 * Namespace: cinux::lib
 */

#pragma once

#include <cassert>
#include <cstddef>

namespace cinux::lib {

/// @brief Non-null pointer wrapper. @tparam T a pointer type (e.g. Task*).
template <typename T>
class NotNull {
public:
    /// Implicit from any non-null pointer; traps if null (the boundary check).
    constexpr NotNull(T ptr) : ptr_(ptr) { assert(ptr_ != nullptr); }

    NotNull(std::nullptr_t)               = delete;  ///< no null construction
    NotNull<T>& operator=(std::nullptr_t) = delete;  ///< no null assignment

    /// Implicit back to the raw pointer -- keeps NotNull a drop-in for code
    /// written against T.
    constexpr operator T() const { return ptr_; }

    constexpr T              get() const { return ptr_; }
    constexpr decltype(auto) operator*() const { return *ptr_; }
    constexpr T              operator->() const { return ptr_; }

private:
    T ptr_;
};

}  // namespace cinux::lib
