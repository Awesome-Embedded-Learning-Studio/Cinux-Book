/**
 * @file    ring_queue.hpp
 * @brief   A bounded ring queue: one empty slot keeps full and empty one
 *          comparison apart, and overflow drops the newcomer.
 *
 * A plain data structure with no opinion about concurrency: on one core
 * with non-reentrant interrupts it needs no lock, elsewhere the caller
 * brings one. Capacity is a template fact, storage lives inline, and
 * construction is constant — safe in freestanding singletons.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_container
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base::container {

/**
 * @brief         A bounded FIFO ring over Capacity-1 usable slots.
 *
 * @tparam        Element   What travels through the queue.
 * @tparam        Capacity  Total slot count; one stays empty so full
 *                          and empty stay distinguishable.
 * @note          Overflow policy: push returns false and the newcomer
 *                          is dropped, the resident queue untouched.
 * @since         0.1.0
 * @ingroup       base_container
 */
template <typename Element, unsigned int Capacity>
class RingQueue {
public:
    /**
     * @brief         Offer one element to the queue.
     *
     * @param[in]     value   Element to store.
     * @return        True when stored; false when the queue was full
     *                and the element was dropped.
     * @since         0.1.0
     * @ingroup       base_container
     */
    bool push(Element value) {
        unsigned int const kNext = advance(head_);
        if (kNext == tail_) {
            return false;
        }
        slots_[head_] = value;
        head_         = kNext;
        return true;
    }

    /**
     * @brief         Take the oldest element.
     *
     * @param[out]    value   Where the element lands.
     * @return        True when an element was there; false when empty.
     * @since         0.1.0
     * @ingroup       base_container
     */
    bool pop(Element& value) {
        if (tail_ == head_) {
            return false;
        }
        value = slots_[tail_];
        tail_ = advance(tail_);
        return true;
    }

    /**
     * @brief         How many elements wait.
     *
     * @return        Waiting element count.
     * @since         0.1.0
     * @ingroup       base_container
     */
    [[nodiscard]] unsigned int count() const { return (head_ + Capacity - tail_) % Capacity; }

private:
    /**
     * @brief         Step one slot forward, wrapping at the end.
     *
     * @param[in]     slot   Current slot index.
     * @return        The next slot index.
     * @since         0.1.0
     * @ingroup       base_container
     */
    static constexpr unsigned int advance(unsigned int slot) { return (slot + 1U) % Capacity; }

    Element      slots_[Capacity]{};
    unsigned int head_ = 0;
    unsigned int tail_ = 0;
};

}  // namespace cinux::base::container
