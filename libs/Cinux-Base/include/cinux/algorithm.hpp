/**
 * @file algorithm.hpp
 * @brief Minimal freestanding algorithm library — replaces <algorithm>.
 */

#ifndef CINUX_ALGORITHM_HPP
#define CINUX_ALGORITHM_HPP

#include <cinux/span.hpp>
#include <cstddef>
#include <utility>

namespace cinux::lib {

// ============================================================
// Constexpr inline utilities
// ============================================================

template <typename T>
constexpr const T& min(const T& a, const T& b) {
    return (b < a) ? b : a;
}

template <typename T>
constexpr const T& max(const T& a, const T& b) {
    return (a < b) ? b : a;
}

template <typename T>
constexpr T clamp(const T& v, const T& lo, const T& hi) {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

template <typename T>
constexpr void swap(T& a, T& b) {
    T tmp = std::move(a);
    a     = std::move(b);
    b     = std::move(tmp);
}

// ============================================================
// Find / find_if (constexpr, inline)
// ============================================================

template <typename T>
constexpr T* find(T* begin, T* end, const T& value) {
    for (T* p = begin; p != end; ++p) {
        if (*p == value) {
            return p;
        }
    }
    return end;
}

template <typename T, typename Pred>
constexpr T* find_if(T* begin, T* end, Pred pred) {
    for (T* p = begin; p != end; ++p) {
        if (pred(*p)) {
            return p;
        }
    }
    return end;
}

// ============================================================
// Fill / copy (constexpr, inline)
// ============================================================

template <typename T>
constexpr void fill(T* begin, T* end, const T& value) {
    for (T* p = begin; p != end; ++p) {
        *p = value;
    }
}

template <typename T>
constexpr T* copy(const T* src_begin, const T* src_end, T* dst) {
    T* d = dst;
    for (const T* s = src_begin; s != src_end; ++s, ++d) {
        *d = *s;
    }
    return d;
}

// ============================================================
// Runtime algorithms (templates — must be in header)
// ============================================================

/** @brief Insertion sort — O(n²), non-recursive, stable. */
template <typename T>
void insertion_sort(T* begin, T* end) {
    if (begin == end) {
        return;
    }
    for (T* i = begin + 1; i != end; ++i) {
        T  key = static_cast<T&&>(*i);
        T* j   = i;
        while (j != begin && *(j - 1) > key) {
            *j = static_cast<T&&>(*(j - 1));
            --j;
        }
        *j = static_cast<T&&>(key);
    }
}

/** @brief Insertion sort with custom comparator. */
template <typename T, typename Comp>
void insertion_sort(T* begin, T* end, Comp comp) {
    if (begin == end) {
        return;
    }
    for (T* i = begin + 1; i != end; ++i) {
        T  key = static_cast<T&&>(*i);
        T* j   = i;
        while (j != begin && comp(key, *(j - 1))) {
            *j = static_cast<T&&>(*(j - 1));
            --j;
        }
        *j = static_cast<T&&>(key);
    }
}

/** @brief Binary search in a sorted range. Returns nullptr if not found. */
template <typename T>
constexpr T* binary_search(T* begin, T* end, const T& value) {
    T* lo = begin;
    T* hi = end;
    while (lo < hi) {
        T* mid = lo + (hi - lo) / 2;
        if (*mid == value) {
            return mid;
        }
        if (*mid < value) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return nullptr;
}

}  // namespace cinux::lib

#endif  // CINUX_ALGORITHM_HPP
