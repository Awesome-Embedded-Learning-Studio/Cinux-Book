/**
 * @file ring_buffer.hpp
 * @brief SPSC ring buffer — single-producer single-consumer, no heap allocation.
 */

#ifndef CINUX_RING_BUFFER_HPP
#define CINUX_RING_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cinux::lib {

/**
 * @brief Fixed-capacity ring buffer for streaming data.
 *
 * @tparam T  Element type.
 * @tparam N  Capacity (number of elements).
 *
 * Uses explicit count_ to distinguish full from empty.
 * push_batch / pop_batch handle wrap-around correctly.
 */
template <typename T, size_t N>
class RingBuffer {
public:
    constexpr RingBuffer() = default;

    constexpr bool   empty() const { return count_ == 0; }
    constexpr bool   full() const { return count_ == N; }
    constexpr size_t size() const { return count_; }
    constexpr size_t capacity() const { return N; }

    // ============================================================
    // Single element
    // ============================================================

    constexpr bool push(const T& item) {
        if (full()) {
            return false;
        }
        buffer_[tail_] = item;
        advance_tail();
        return true;
    }

    constexpr bool pop(T& out) {
        if (empty()) {
            return false;
        }
        out = buffer_[head_];
        advance_head();
        return true;
    }

    constexpr void clear() {
        head_  = 0;
        tail_  = 0;
        count_ = 0;
    }

    constexpr const T& peek_front() const { return buffer_[head_]; }
    constexpr const T& peek_back() const { return buffer_[(tail_ + N - 1) % N]; }

    // ============================================================
    // Batch operations
    // ============================================================

    /** @brief Push multiple items. Returns number actually pushed. */
    size_t push_batch(const T* items, size_t count) {
        size_t pushed = 0;
        while (pushed < count && !full()) {
            buffer_[tail_] = items[pushed++];
            advance_tail();
        }
        return pushed;
    }

    /** @brief Pop multiple items. Returns number actually popped. */
    size_t pop_batch(T* items, size_t count) {
        size_t popped = 0;
        while (popped < count && !empty()) {
            items[popped++] = buffer_[head_];
            advance_head();
        }
        return popped;
    }

private:
    constexpr void advance_tail() {
        tail_ = (tail_ + 1) % N;
        ++count_;
    }

    constexpr void advance_head() {
        head_ = (head_ + 1) % N;
        --count_;
    }

    T      buffer_[N]{};
    size_t head_  = 0;
    size_t tail_  = 0;
    size_t count_ = 0;
};

template <size_t N>
using ByteRingBuffer = RingBuffer<uint8_t, N>;

}  // namespace cinux::lib

#endif  // CINUX_RING_BUFFER_HPP
