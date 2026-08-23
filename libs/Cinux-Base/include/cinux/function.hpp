/**
 * @file function.hpp
 * @brief Type-erased callable with Small Buffer Optimization — no heap allocation.
 *
 * Stores callable objects up to InlineSize bytes inline. Larger callables
 * trigger a compile-time error via static_assert.
 */

#ifndef CINUX_FUNCTION_HPP
#define CINUX_FUNCTION_HPP

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace cinux::lib {

/**
 * @brief Type-erased callable wrapper with inline storage.
 *
 * @tparam R          Return type.
 * @tparam Args       Argument types.
 * @tparam InlineSize Size of the inline buffer in bytes (default 32).
 *
 * @code
 *   Function<int(int)> f = [](int x) { return x * 2; };
 *   int result = f(21);  // result == 42
 * @endcode
 */
template <typename Sig, size_t InlineSize = 32>
class Function;  // primary template — not defined

template <typename R, typename... Args, size_t InlineSize>
class Function<R(Args...), InlineSize> {
public:
    constexpr Function() = default;

    constexpr Function(std::nullptr_t) {}  // NOLINT

    /** @brief Construct from a function pointer. */
    constexpr Function(R (*fn)(Args...)) {  // NOLINT
        if (fn) {
            using FnPtr = R (*)(Args...);
            new (buf_) FnPtr(fn);
            invoke_  = &invoke_fnptr;
            destroy_ = &destroy_noop;
            copy_    = &copy_fnptr;
        }
    }

    /** @brief Construct from a callable object (lambda, functor). */
    template <typename F,
              typename = std::enable_if_t<!std::is_same<std::decay_t<F>, Function>::value>>
    constexpr Function(F&& f) {  // NOLINT
        using Decayed = std::decay_t<F>;
        static_assert(sizeof(Decayed) <= InlineSize,
                      "Function: callable exceeds InlineSize — capture less data or increase N");
        static_assert(std::is_trivially_copyable<Decayed>::value ||
                          std::is_nothrow_move_constructible<Decayed>::value,
                      "Function: callable must be trivially copyable or nothrow moveable");

        new (buf_) Decayed(std::forward<F>(f));
        invoke_  = &invoke_obj<Decayed>;
        destroy_ = &destroy_obj<Decayed>;
        copy_    = &copy_obj<Decayed>;
    }

    constexpr Function(const Function& other)
        : invoke_(other.invoke_), destroy_(other.destroy_), copy_(other.copy_) {
        if (copy_) {
            copy_(buf_, other.buf_);
        }
    }

    constexpr Function(Function&& other) noexcept
        : invoke_(other.invoke_), destroy_(other.destroy_), copy_(other.copy_) {
        if (other.copy_) {
            copy_(buf_, other.buf_);
        }
        other.reset();
    }

    constexpr Function& operator=(const Function& other) {
        if (this != &other) {
            reset();
            invoke_  = other.invoke_;
            destroy_ = other.destroy_;
            copy_    = other.copy_;
            if (copy_) {
                copy_(buf_, other.buf_);
            }
        }
        return *this;
    }

    constexpr Function& operator=(Function&& other) noexcept {
        if (this != &other) {
            reset();
            invoke_  = other.invoke_;
            destroy_ = other.destroy_;
            copy_    = other.copy_;
            if (copy_) {
                copy_(buf_, other.buf_);
            }
            other.reset();
        }
        return *this;
    }

    ~Function() {
        if (destroy_) {
            destroy_(buf_);
        }
    }

    R operator()(Args... args) const { return invoke_(buf_, std::forward<Args>(args)...); }

    constexpr bool     valid() const { return invoke_ != nullptr; }
    constexpr explicit operator bool() const { return valid(); }

    constexpr void reset() {
        if (destroy_) {
            destroy_(buf_);
        }
        invoke_  = nullptr;
        destroy_ = nullptr;
        copy_    = nullptr;
    }

private:
    // Type-erased function pointers
    using InvokeFn  = R (*)(const uint8_t*, Args...);
    using DestroyFn = void (*)(uint8_t*);
    using CopyFn    = void (*)(uint8_t* dst, const uint8_t* src);

    // Function pointer trampolines
    static R invoke_fnptr(const uint8_t* buf, Args... args) {
        return (*reinterpret_cast<R (*const*)(Args...)>(buf))(std::forward<Args>(args)...);
    }

    static void destroy_noop(uint8_t*) {}

    static void copy_fnptr(uint8_t* dst, const uint8_t* src) {
        *reinterpret_cast<R (**)(Args...)>(dst) = *reinterpret_cast<R (*const*)(Args...)>(src);
    }

    // Object trampolines
    template <typename F>
    static R invoke_obj(const uint8_t* buf, Args... args) {
        return (*reinterpret_cast<const F*>(buf))(std::forward<Args>(args)...);
    }

    template <typename F>
    static void destroy_obj(uint8_t* buf) {
        reinterpret_cast<F*>(buf)->~F();
    }

    template <typename F>
    static void copy_obj(uint8_t* dst, const uint8_t* src) {
        new (dst) F(*reinterpret_cast<const F*>(src));
    }

    alignas(max_align_t) uint8_t buf_[InlineSize]{};
    InvokeFn  invoke_  = nullptr;
    DestroyFn destroy_ = nullptr;
    CopyFn    copy_    = nullptr;
};

}  // namespace cinux::lib

#endif  // CINUX_FUNCTION_HPP
