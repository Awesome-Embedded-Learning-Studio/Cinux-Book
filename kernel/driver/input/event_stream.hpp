/**
 * @file    event_stream.hpp
 * @brief   One input device's event ledger: IRQ hands in, a reader
 *          walks out, the grab word decides who else hears.
 *
 * The stream is the whole device half of an input node. Producers run
 * in interrupt context — one push per word, integer-only, nothing slow
 * — and consumers read whole wire records through the device face.
 * The grab hook is the seat Linux's EVIOCGRAB fills: one owner may ask
 * the device to stop feeding its other consumers, and the keyboard is
 * the device that needs the seat, so the hook travels as a plain
 * function pointer the wiring registers.
 *
 * @author  Charliechen114514
 * @date    2026-10-26
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <array>

#include "cinux/container/ring_queue.hpp"
#include "cinux/result.hpp"
#include "cinux/spin_mutex.hpp"
#include "kernel/driver/input/input_config.hpp"

namespace cinux::driver {

/// What a device does when its stream is grabbed or released.
using InputGrabHook = void (*)(void* context, bool grabbed);

/**
 * @brief   One input device's ledger: words in from interrupt context,
 *          whole records out to a reader.
 * @since   0.9.0
 * @ingroup kernel_driver
 */
class EventStream {
public:
    EventStream() = default;

    EventStream(const EventStream&)            = delete;
    EventStream& operator=(const EventStream&) = delete;

    /**
     * @brief         Hands one event word in, stamped by the kernel
     *                clock and queued behind whatever waits.
     *
     * @param[in]     type    The event's class word.
     * @param[in]     code    The event's member word.
     * @param[in]     value   The event's payload word.
     * @return        None.
     * @note          Interrupt-context safe: queue push, nothing slow.
     *                Overflow starts a SYN_DROPPED recovery with releases for
     *                delivered keys, followed by the latest report.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    void publish(uint16_t type, uint16_t code, int32_t value);

    /**
     * @brief         Hands the report-closing word in.
     *
     * @return        None.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    void publish_report();

    /**
     * @brief         Turns the stream into absolute axes with one range.
     *
     * @param[in]     maximum   The top of both axes' range, zero based.
     * @return        None
     * @note          After this word the stream answers the absolute-axis
     *                question and replays both axes' positions after a
     *                dropped burst.
     * @warning       None
     * @throws        None
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    void configure_absolute(int32_t maximum);

    /**
     * @brief         Registers the drain a reader's read runs first.
     *
     * @param[in]     hook   The function polled before every read and poll.
     * @return        None
     * @note          Devices whose reports wait on a drained ring hand
     *                their drain over here, so a reader pulls reports
     *                without an interrupt line.
     * @warning       None
     * @throws        None
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    void set_poll_hook(void (*hook)());

    /**
     * @brief         Reads whole wire records out, oldest first.
     *
     * @param[out]    sink    Where the records land.
     * @param[in]     count   Bytes the reader offered.
     * @return        Bytes handed over, or kWouldBlock when nothing
     *                waits.
     * @note          A partial record never crosses the line: only
     *                whole multiples of the record size count.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] base::Result<unsigned long> read(void* sink, unsigned long count,
                                                   const void* owner = nullptr);

    /**
     * @brief         Whether at least one whole record waits.
     *
     * @return        True when a reader would make progress now.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool poll(const void* owner = nullptr) const;

    /**
     * @brief         Answers the grab question.
     *
     * @param[in]     word    The question word.
     * @param[in]     argument   The grab flag's value.
     * @return        Zero on the grab word, a negative errno otherwise.
     * @note          An independent open owns a grab. An existing grab returns
     *                EBUSY, including its owner's repeat. A foreign release
     *                returns EINVAL; final close releases the grab.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] long long answer(const void* owner, unsigned long long word,
                                   unsigned long long argument);

    /**
     * @brief         Seats the hook the grab word wakes.
     *
     * @param[in]     hook     The hook, or nullptr for none.
     * @param[in]     context  The word the hook receives back.
     * @return        None.
     * @note          Bring-up wiring, before the node goes live.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    void set_grab_hook(InputGrabHook hook, void* context);

    /**
     * @brief Release a grab when its owning open description retires.
     * @param[in] owner Identity used for the grab.
     */
    void release_owner(const void* owner);

private:
    base::container::RingQueue<InputEventWire, kInputEventQueueCapacity> events_{};
    InputGrabHook                                                        grab_hook_ = nullptr;
    void*                                                                hook_word_ = nullptr;
    mutable base::SpinMutex                                              gate_{};
    const void*                                                          owner_ = nullptr;
    std::array<bool, kInputKeyCodeCount>                                 current_keys_{};
    std::array<bool, kInputKeyCodeCount>                                 delivered_keys_{};
    void (*poll_hook_)()                  = nullptr;
    bool                        absolute_ = false;
    std::array<InputAbsInfo, 2> axes_{};
    unsigned long               recovery_axis_ = 0;
    bool                        dropped_       = false;
    bool                        recovering_    = false;
    unsigned long               recovery_key_  = 0;
};

}  // namespace cinux::driver
