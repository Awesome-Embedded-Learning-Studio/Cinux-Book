/**
 * @file string_view.hpp
 * @brief Zero-allocation string view — non-owning, not assuming null termination.
 */

#ifndef CINUX_STRING_VIEW_HPP
#define CINUX_STRING_VIEW_HPP

#include <cstddef>
#include <cstdint>

namespace cinux::lib {

/**
 * @brief Non-owning view into a character sequence.
 *
 * Does not assume null termination. Safe for substrings of larger buffers.
 */
class StringView {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    constexpr StringView() = default;
    constexpr StringView(const char* str) : data_(str), size_(str ? len_(str) : 0) {}
    constexpr StringView(const char* str, size_t len) : data_(str), size_(len) {}

    // ============================================================
    // Accessors
    // ============================================================

    constexpr size_t      size() const { return size_; }
    constexpr bool        empty() const { return size_ == 0; }
    constexpr const char* data() const { return data_; }

    constexpr char operator[](size_t i) const { return (i < size_) ? data_[i] : '\0'; }

    constexpr char front() const { return size_ > 0 ? data_[0] : '\0'; }
    constexpr char back() const { return size_ > 0 ? data_[size_ - 1] : '\0'; }

    // ============================================================
    // Comparison
    // ============================================================

    constexpr int compare(StringView o) const {
        size_t len = size_ < o.size_ ? size_ : o.size_;
        for (size_t i = 0; i < len; ++i) {
            auto a = static_cast<unsigned char>(data_[i]);
            auto b = static_cast<unsigned char>(o.data_[i]);
            if (a < b) {
                return -1;
            }
            if (a > b) {
                return 1;
            }
        }
        if (size_ < o.size_) {
            return -1;
        }
        if (size_ > o.size_) {
            return 1;
        }
        return 0;
    }

    constexpr bool equals(StringView o) const { return compare(o) == 0; }

    constexpr bool operator==(StringView o) const { return compare(o) == 0; }
    constexpr bool operator!=(StringView o) const { return compare(o) != 0; }
    constexpr bool operator<(StringView o) const { return compare(o) < 0; }
    constexpr bool operator<=(StringView o) const { return compare(o) <= 0; }
    constexpr bool operator>(StringView o) const { return compare(o) > 0; }
    constexpr bool operator>=(StringView o) const { return compare(o) >= 0; }

    // ============================================================
    // Predicates
    // ============================================================

    constexpr bool starts_with(StringView prefix) const {
        if (prefix.size_ > size_) {
            return false;
        }
        return StringView(data_, prefix.size_).equals(prefix);
    }

    constexpr bool ends_with(StringView suffix) const {
        if (suffix.size_ > size_) {
            return false;
        }
        return StringView(data_ + size_ - suffix.size_, suffix.size_).equals(suffix);
    }

    // ============================================================
    // Search
    // ============================================================

    constexpr size_t find(char c, size_t pos = 0) const {
        for (size_t i = pos; i < size_; ++i) {
            if (data_[i] == c) {
                return i;
            }
        }
        return npos;
    }

    constexpr size_t find(StringView needle, size_t pos = 0) const {
        if (needle.size_ == 0) {
            return pos <= size_ ? pos : npos;
        }
        if (needle.size_ > size_) {
            return npos;
        }
        for (size_t i = pos; i + needle.size_ <= size_; ++i) {
            if (StringView(data_ + i, needle.size_).equals(needle)) {
                return i;
            }
        }
        return npos;
    }

    constexpr size_t rfind(char c) const {
        for (size_t i = size_; i > 0; --i) {
            if (data_[i - 1] == c) {
                return i - 1;
            }
        }
        return npos;
    }

    // ============================================================
    // Substring
    // ============================================================

    constexpr StringView substr(size_t pos, size_t count = npos) const {
        if (pos >= size_) {
            return {};
        }
        size_t remaining = size_ - pos;
        size_t len       = (count < remaining) ? count : remaining;
        return {data_ + pos, len};
    }

private:
    const char* data_ = nullptr;
    size_t      size_ = 0;

    static constexpr size_t len_(const char* s) {
        size_t n = 0;
        while (s && s[n]) {
            ++n;
        }
        return n;
    }
};

}  // namespace cinux::lib

#endif  // CINUX_STRING_VIEW_HPP
