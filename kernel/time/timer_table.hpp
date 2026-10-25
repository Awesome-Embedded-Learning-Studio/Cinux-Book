/**
 * @file    timer_table.hpp
 * @brief   The kernel's timer service: seats that fire when time says so.
 *
 * A consumer arms a seat with a deadline and a call to make; the table
 * keeps the seat until the monotonic now walks past the deadline, then
 * makes the call, once. The service knows nothing about who keeps time
 * or why a seat exists — advance(now) is fed by whichever context owns
 * the clock, and the call is whatever erased pair the consumer armed.
 * Seats are taken, not recycled silently: a ticket carries the seat
 * number and the seat's generation, so a stale handle cancels nothing.
 * A seat is disarmed before its call runs, so a callback that re-arms
 * itself (a retransmission timer does exactly that) or cancels its own
 * seat cannot see a half-dead seat or fire twice.
 *
 * @author  Charliechen114514
 * @date    2026-10-24
 * @version 0.1
 * @since   0.7.0
 * @ingroup kernel_time
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>

#include "cinux/result.hpp"
#include "cinux/singleton.hpp"
#include "kernel/time/timer_config.hpp"

namespace cinux::time {

/**
 * @brief   One armed seat's handle: a seat number and the seat's generation.
 * @since   0.7.0
 * @ingroup kernel_time
 */
struct TimerTicket {
    /// Which seat in the table.
    unsigned long slot  = 0;
    /// The seat's generation when the ticket was minted.
    unsigned long stamp = 0;
};

/**
 * @brief   The timer service: fixed seats, deadlines, one call each.
 * @note    Meyers-singleton shape after Tick; the instance is
 *          constant-initialized, so the zero-construction rule holds.
 *          Exhaustion is refused by name, never queued.
 * @since   0.7.0
 * @ingroup kernel_time
 */
class TimerTable : public cinux::base::Singleton<TimerTable> {
    friend class cinux::base::Singleton<TimerTable>;

public:
    /**
     * @brief         Arms one seat to fire at or after the deadline.
     *
     * @param[in]     context       Handed back to the call untouched.
     * @param[in]     call          What runs when the deadline lands.
     * @param[in]     deadline_ns   The monotonic now that fires the seat.
     * @return        The seat's ticket, or the refusal.
     * @note          Arming a deadline already in the past is legal: the
     *                next advance() past it fires the seat.
     * @since         0.7.0
     * @ingroup       kernel_time
     */
    base::Result<TimerTicket, base::KernelError> arm(void* context, void (*call)(void* context),
                                                     TimerDeadline deadline_ns);

    /**
     * @brief         Disarms a seat before its deadline.
     *
     * @param[in]     ticket   The ticket arm() answered.
     * @return        True when a live seat sat there; false for a stale
     *                or already-fired ticket.
     * @since         0.7.0
     * @ingroup       kernel_time
     */
    bool cancel(const TimerTicket& ticket);

    /**
     * @brief         Fires every seat whose deadline has landed.
     *
     * @param[in]     now   The monotonic now, in nanoseconds.
     * @return        None
     * @note          Each firing seat is disarmed before its call runs,
     *                so the call may arm or cancel seats freely, this
     *                seat's included. Calls run in seat order, one pass,
     *                no recursion into advance itself.
     * @since         0.7.0
     * @ingroup       kernel_time
     */
    void advance(TimerDeadline now);

private:
    TimerTable() = default;

    /**
     * @brief   One armed seat: the erased call and its generation.
     * @since   0.7.0
     * @ingroup kernel_time
     */
    struct Seat {
        bool          used          = false;
        unsigned long stamp         = 0;
        void*         context       = nullptr;
        void (*call)(void* context) = nullptr;
        TimerDeadline deadline_ns   = 0;
    };

    Seat seats_[kTimerSeats] = {};
};

}  // namespace cinux::time
