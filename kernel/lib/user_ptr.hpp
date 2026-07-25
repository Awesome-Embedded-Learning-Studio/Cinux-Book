/**
 * @file kernel/lib/user_ptr.hpp
 * @brief UserPtr<T> — zero-overhead marker for pointers originating from user
 *        space (aligned with the Linux sparse `__user` annotation).
 *
 * Like NotNull, this is a one-pointer, no-extra-state contract wrapper that
 * encodes a property in the type system without changing runtime behaviour.
 * Where NotNull asserts "never null", UserPtr asserts "this pointer came from
 * user space (untrusted; must be validated and touched only across the
 * user/kernel boundary)". It models the Linux sparse `__user` annotation,
 * which is a compile-time marker (sparse checks that __user pointers are only
 * accessed through copy_to_user/copy_from_user), not a runtime check.
 *
 * Unlike NotNull, UserPtr may hold nullptr: a user syscall argument can
 * legitimately be NULL (the syscall then returns -EFAULT), so nullness is a
 * runtime validation concern, not a type-level invariant.
 *
 * SCOPE / BOUNDARIES (read before reuse):
 * - **Marker only — no validation, no copy.** Touching a UserPtr's pointee
 *   directly is exactly as (un)safe as dereferencing the raw pointer. Runtime
 *   canonical-address validation stays with the existing
 *   cinux::syscall::validate_user_ptr() (unchanged in F-QA Q4a). Future
 *   access_ok + copy_to_user/copy_from_user (DEBT-019) will key off UserPtr.
 * - **HONEST LIMITATION:** introducing UserPtr does NOT, in Q4a, make user
 *   pointers safer — it has no consumer yet. It is type-first scaffolding for
 *   the DEBT-019 fix and for annotating syscall boundaries. Do not mistake its
 *   presence for "user pointer issues are handled".
 *
 * Lives in kernel/lib (cinux::lib), like NotNull/kprintf. It deliberately does
 * NOT call or include cinux::syscall::validate_user_ptr() — that would create a
 * lib->syscall reverse dependency. Callers pass the raw pointer to
 * validate_user_ptr() themselves (e.g. `validate_user_ptr(up.get())`).
 *
 * Namespace: cinux::lib
 */

#pragma once

namespace cinux::lib {

/// @brief Zero-overhead marker for a pointer originating from user space.
/// @tparam T  a pointer type (e.g. `void*`, `int*`, `const char*`).
///
/// See @file header for scope, boundaries, and the __user alignment rationale.
template <typename T>
class UserPtr {
public:
    /// Default-constructs to nullptr (a user arg may be NULL).
    constexpr UserPtr() = default;

    /// Implicit from any user-supplied pointer (including nullptr — user may
    /// pass NULL, which the syscall handles as -EFAULT). No check here.
    constexpr UserPtr(T ptr) : ptr_(ptr) {}

    /// Implicit back to the raw pointer — keeps UserPtr a drop-in for code
    /// written against T (e.g. passing to validate_user_ptr).
    constexpr operator T() const { return ptr_; }

    constexpr T              get() const { return ptr_; }
    constexpr T              operator->() const { return ptr_; }
    constexpr decltype(auto) operator*() const { return *ptr_; }

private:
    T ptr_{};
};

}  // namespace cinux::lib
