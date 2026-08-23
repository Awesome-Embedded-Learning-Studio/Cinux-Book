/**
 * @file buffer.hpp
 * @brief Type-safe byte buffer — read-only view + fixed-capacity owning container.
 */

#ifndef CINUX_BUFFER_HPP
#define CINUX_BUFFER_HPP

#include <cinux/span.hpp>
#include <cinux/string_view.hpp>
#include <cstddef>
#include <cstring>

namespace cinux::lib {

/**
 * @brief Non-owning read-only view into a byte sequence.
 */
class BufferView {
public:
    constexpr BufferView() = default;
    constexpr BufferView(const void* data, size_t size)
        : data_(static_cast<const uint8_t*>(data)), size_(size) {}

    constexpr const uint8_t* data() const { return data_; }
    constexpr size_t         size() const { return size_; }
    constexpr bool           empty() const { return size_ == 0; }

    constexpr const uint8_t& operator[](size_t i) const { return data_[i]; }

    constexpr BufferView slice(size_t offset, size_t len) const {
        if (offset >= size_) {
            return {};
        }
        size_t available = size_ - offset;
        size_t actual    = (len < available) ? len : available;
        return {data_ + offset, actual};
    }

    /** @brief View the bytes as a string. Not constexpr due to reinterpret_cast. */
    StringView as_string() const { return StringView(reinterpret_cast<const char*>(data_), size_); }

private:
    const uint8_t* data_ = nullptr;
    size_t         size_ = 0;
};

/**
 * @brief Fixed-capacity owning byte buffer.
 *
 * @tparam N  Capacity in bytes.
 */
template <size_t N>
class StaticBuffer {
public:
    constexpr StaticBuffer() = default;

    constexpr uint8_t*       data() { return data_; }
    constexpr const uint8_t* data() const { return data_; }
    static constexpr size_t  capacity() { return N; }
    constexpr size_t         size() const { return size_; }
    constexpr bool           empty() const { return size_ == 0; }

    constexpr void resize(size_t new_size) {
        // In freestanding: caller must ensure new_size <= N
        if (new_size > N) {
            return;  // safe fallback
        }
        size_ = new_size;
    }

    constexpr void fill(uint8_t value) {
        for (size_t i = 0; i < N; ++i) {
            data_[i] = value;
        }
        size_ = N;
    }

    void copy_from(const void* src, size_t len) {
        size_t copy = (len < N) ? len : N;
        memcpy(data_, src, copy);
        size_ = copy;
    }

    void copy_to(void* dst, size_t len) const {
        size_t copy = (len < size_) ? len : size_;
        memcpy(dst, data_, copy);
    }

    constexpr BufferView view() const { return {data_, size_}; }

    constexpr ByteSpan      as_span() { return {data_, size_}; }
    constexpr ConstByteSpan as_span() const { return {data_, size_}; }

private:
    uint8_t data_[N]{};
    size_t  size_ = 0;
};

}  // namespace cinux::lib

#endif  // CINUX_BUFFER_HPP
