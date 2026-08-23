/**
 * @file span.hpp
 * @brief Non-owning contiguous memory view — std::span for C++17.
 */

#ifndef CINUX_SPAN_HPP
#define CINUX_SPAN_HPP

#include <cstddef>
#include <cstdint>

namespace cinux::lib {

/**
 * @brief Non-owning view over a contiguous sequence of T.
 *
 * @tparam T  Element type.
 *
 * Provides iterator support for range-for. No bounds checking in release.
 */
template <typename T>
class Span {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    constexpr Span() = default;
    constexpr Span(T* data, size_t size) : data_(data), size_(size) {}
    constexpr Span(T* begin, T* end) : data_(begin), size_(end - begin) {}

    template <size_t N>
    constexpr Span(T (&arr)[N]) : data_(arr), size_(N) {}  // NOLINT

    // ============================================================
    // Accessors
    // ============================================================

    constexpr size_t size() const { return size_; }
    constexpr bool   empty() const { return size_ == 0; }
    constexpr T*     data() const { return data_; }

    constexpr T& operator[](size_t i) const { return data_[i]; }
    constexpr T& front() const { return data_[0]; }
    constexpr T& back() const { return data_[size_ - 1]; }

    // ============================================================
    // Subviews
    // ============================================================

    constexpr Span first(size_t count) const { return {data_, count < size_ ? count : size_}; }

    constexpr Span last(size_t count) const {
        size_t off = (count < size_) ? size_ - count : 0;
        return {data_ + off, size_ - off};
    }

    constexpr Span subspan(size_t pos, size_t count = npos) const {
        if (pos >= size_) {
            return {};
        }
        size_t remaining = size_ - pos;
        size_t len       = (count < remaining) ? count : remaining;
        return {data_ + pos, len};
    }

    // ============================================================
    // Iterators
    // ============================================================

    constexpr T* begin() const { return data_; }
    constexpr T* end() const { return data_ + size_; }

private:
    T*     data_ = nullptr;
    size_t size_ = 0;
};

// ============================================================
// Type aliases
// ============================================================

using ByteSpan      = Span<uint8_t>;
using ConstByteSpan = Span<const uint8_t>;

}  // namespace cinux::lib

#endif  // CINUX_SPAN_HPP
