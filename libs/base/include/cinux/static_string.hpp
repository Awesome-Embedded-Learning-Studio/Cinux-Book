/**
 * @file static_string.hpp
 * @brief Fixed-capacity null-terminated string — for paths, filenames, device identifiers.
 */

#ifndef CINUX_STATIC_STRING_HPP
#define CINUX_STATIC_STRING_HPP

#include <cinux/string_view.hpp>
#include <cstddef>
#include <cstring>

namespace cinux::lib {

/**
 * @brief Fixed-capacity string with null termination.
 *
 * @tparam N  Maximum characters including null terminator.
 *
 * Never heap-allocates. Excess append operations return false.
 */
template <size_t N>
class StaticString {
public:
    constexpr StaticString() : size_(0) { data_[0] = '\0'; }

    constexpr StaticString(const char* str) : size_(0) {
        data_[0] = '\0';
        if (str) {
            while (*str && size_ < N - 1) {
                data_[size_++] = *str++;
            }
            data_[size_] = '\0';
        }
    }

    constexpr StaticString(const char* str, size_t len) : size_(0) {
        data_[0]    = '\0';
        size_t copy = (len < N - 1) ? len : N - 1;
        for (size_t i = 0; i < copy; ++i) {
            data_[size_++] = str[i];
        }
        data_[size_] = '\0';
    }

    constexpr StaticString(StringView sv) : StaticString(sv.data(), sv.size()) {}  // NOLINT

    // ============================================================
    // Accessors
    // ============================================================

    static constexpr size_t capacity() { return N; }
    constexpr size_t        size() const { return size_; }
    constexpr bool          empty() const { return size_ == 0; }
    constexpr bool          full() const { return size_ == N - 1; }
    constexpr const char*   c_str() const { return data_; }

    constexpr char  operator[](size_t i) const { return (i < N) ? data_[i] : '\0'; }
    constexpr char& operator[](size_t i) { return data_[i]; }

    // ============================================================
    // Modification
    // ============================================================

    constexpr void clear() {
        size_    = 0;
        data_[0] = '\0';
    }

    constexpr bool append(char c) {
        if (size_ >= N - 1) {
            return false;
        }
        data_[size_++] = c;
        data_[size_]   = '\0';
        return true;
    }

    bool append(StringView sv) {
        for (size_t i = 0; i < sv.size(); ++i) {
            if (!append(sv[i])) {
                return false;
            }
        }
        return true;
    }

    bool append(const char* str) { return append(StringView(str)); }

    constexpr void truncate(size_t new_size) {
        if (new_size < size_) {
            size_        = new_size;
            data_[size_] = '\0';
        }
    }

    // ============================================================
    // Comparison
    // ============================================================

    constexpr bool equals(StringView sv) const { return view().equals(sv); }

    constexpr bool operator==(StringView sv) const { return equals(sv); }
    constexpr bool operator==(const StaticString& o) const { return view() == o.view(); }

    // ============================================================
    // Conversion
    // ============================================================

    constexpr StringView view() const { return {data_, size_}; }
    constexpr            operator StringView() const { return view(); }  // NOLINT

    // ============================================================
    // Path utilities
    // ============================================================

    /** @brief Get parent path: "/foo/bar" -> "/foo", "/" -> "/", "file" -> "". */
    constexpr StaticString parent_path() const {
        if (size_ <= 1) {
            return *this;
        }

        // Find last '/' excluding trailing slash
        size_t last_slash = size_;
        for (size_t i = size_; i > 0; --i) {
            if (data_[i - 1] == '/') {
                last_slash = i - 1;
                break;
            }
        }

        // No slash found — no parent
        if (last_slash == size_) {
            return StaticString();
        }

        if (last_slash == 0) {
            return StaticString("/");
        }
        return StaticString(data_, last_slash);
    }

    /** @brief Get filename: "/foo/bar" -> "bar", "/" -> "", "file" -> "file". */
    constexpr StringView filename() const {
        if (size_ == 0) {
            return {};
        }

        for (size_t i = size_; i > 0; --i) {
            if (data_[i - 1] == '/') {
                StringView result = view().substr(i);
                // "/" alone -> return empty (the trailing slash is not a filename)
                if (i == 1 && size_ == 1) {
                    return {};
                }
                return result;
            }
        }
        return view();
    }

private:
    char   data_[N]{};
    size_t size_ = 0;
};

// ============================================================
// Common aliases
// ============================================================

using PathString = StaticString<256>;
using NameString = StaticString<64>;

}  // namespace cinux::lib

#endif  // CINUX_STATIC_STRING_HPP
