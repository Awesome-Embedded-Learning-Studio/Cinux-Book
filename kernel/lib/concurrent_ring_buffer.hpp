/**
 * @file kernel/lib/concurrent_ring_buffer.hpp
 * @brief MPSC ring buffer -- Cinux-Base RingBuffer wrapped with an IRQ-safe Spinlock
 *
 * ConcurrentRingBuffer adapts the freestanding cinux::lib::RingBuffer for
 * kernel multi-producer / multi-consumer use by serialising every operation
 * with cinux::proc::Spinlock::irq_guard() (disables interrupts, then acquires).
 * This makes it safe to call from IRQ context (e.g. a log sink firing inside
 * an interrupt handler) as well as from multiple tasks.
 *
 * The underlying ring buffer is not thread-safe (Cinux-Base contract); all
 * concurrency is handled here.  Capacity is N with no slot sacrificed
 * (RingBuffer tracks an explicit count_).
 *
 * Namespace: cinux::lib
 */

#pragma once

#include <cinux/ring_buffer.hpp>
#include <cstddef>
#include <cstdint>

#include "kernel/proc/sync.hpp"

namespace cinux::lib {

/**
 * @brief Thread-safe (IRQ-safe MPSC) ring buffer
 *
 * @tparam T  Element type (must be default-constructible and copy-assignable).
 * @tparam N  Capacity in elements.
 *
 * Every operation takes the lock with irq_guard(), so it is safe to call from
 * interrupt context and from multiple producers/consumers.  Critical sections
 * are short (a single ring-buffer op), matching the Spinlock usage contract
 * (never held across a blocking operation).
 */
template <typename T, std::size_t N>
class ConcurrentRingBuffer {
public:
    constexpr ConcurrentRingBuffer() = default;

    // -- Single element -------------------------------------------------

    /**
     * @brief Push one item
     * @return true if pushed, false if the buffer is full (item dropped)
     */
    bool push(const T& item) {
        auto guard = lock_.irq_guard();
        return buf_.push(item);
    }

    /**
     * @brief Pop one item
     * @param out  Filled with the front item on success
     * @return true if an item was popped, false if empty
     */
    bool pop(T& out) {
        auto guard = lock_.irq_guard();
        return buf_.pop(out);
    }

    // -- Batch ----------------------------------------------------------

    /**
     * @brief Push up to @p count items, handling wrap-around
     * @return Number of items actually pushed (0..count)
     */
    std::size_t push_batch(const T* items, std::size_t count) {
        auto guard = lock_.irq_guard();
        return buf_.push_batch(items, count);
    }

    /**
     * @brief Pop up to @p count items, handling wrap-around
     * @return Number of items actually popped (0..count)
     */
    std::size_t pop_batch(T* items, std::size_t count) {
        auto guard = lock_.irq_guard();
        return buf_.pop_batch(items, count);
    }

    // -- State ----------------------------------------------------------

    /** @brief Remove all items. */
    void clear() {
        auto guard = lock_.irq_guard();
        buf_.clear();
    }

    /** @brief Whether the buffer holds no items. */
    bool empty() const {
        auto guard = lock_.irq_guard();
        return buf_.empty();
    }

    /** @brief Whether the buffer has no free slot. */
    bool full() const {
        auto guard = lock_.irq_guard();
        return buf_.full();
    }

    /** @brief Number of items currently buffered. */
    std::size_t size() const {
        auto guard = lock_.irq_guard();
        return buf_.size();
    }

    /** @brief Capacity (compile-time constant -- no lock needed). */
    static constexpr std::size_t capacity() { return N; }

private:
    cinux::lib::RingBuffer<T, N>  buf_;
    mutable cinux::proc::Spinlock lock_;
};

}  // namespace cinux::lib
