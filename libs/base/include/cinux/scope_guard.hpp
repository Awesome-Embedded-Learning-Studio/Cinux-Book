/**
 * @file scope_guard.hpp
 * @brief RAII scope cleanup utility — runs a callback on scope exit.
 *
 * Move-only; use SCOPE_EXIT macro for concise defer-style cleanup.
 */

#ifndef CINUX_SCOPE_GUARD_HPP
#define CINUX_SCOPE_GUARD_HPP

#include <type_traits>
#include <utility>

namespace cinux::lib {

/**
 * @brief RAII guard that invokes a callable on destruction unless dismissed.
 *
 * @tparam F  Callable type (lambda, function pointer, functor).
 *
 * Typical usage via the SCOPE_EXIT macro:
 * @code
 *   auto* mapping = mmap(...);
 *   SCOPE_EXIT(munmap(mapping));
 *   // ... use mapping ...
 *   // munmap(mapping) called when scope exits
 * @endcode
 */
template <typename F>
class ScopeGuard {
public:
    /** @brief Construct from a callable. */
    explicit constexpr ScopeGuard(F&& fn) : fn_(std::move(fn)), active_(true) {}

    /** @brief Move constructor — transfers ownership, source becomes inactive. */
    constexpr ScopeGuard(ScopeGuard&& other) noexcept
        : fn_(std::move(other.fn_)), active_(other.active_) {
        other.active_ = false;
    }

    ScopeGuard(const ScopeGuard&)            = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&)      = delete;

    /** @brief Destructor — invokes fn_() unless dismissed. */
    ~ScopeGuard() {
        if (active_) {
            fn_();
        }
    }

    /** @brief Dismiss the guard — fn_() will NOT be called on destruction. */
    constexpr void dismiss() noexcept { active_ = false; }

private:
    F    fn_;
    bool active_;
};

// ============================================================
// Convenience macro
// ============================================================

#define CINUX_SCOPE_CONCAT2_(a, b) a##b
#define CINUX_SCOPE_CONCAT_(a, b)  CINUX_SCOPE_CONCAT2_(a, b)

/**
 * @brief Declare a scope-exit action.
 * @param expr  Statement to execute when the enclosing scope exits.
 *
 * @code
 *   int* p = new int(42);
 *   SCOPE_EXIT(delete p);
 * @endcode
 */
#define SCOPE_EXIT(expr)                                                                           \
    auto CINUX_SCOPE_CONCAT_(_scope_guard_, __LINE__) = ::cinux::lib::ScopeGuard([&]() { expr; })

}  // namespace cinux::lib

#endif  // CINUX_SCOPE_GUARD_HPP
