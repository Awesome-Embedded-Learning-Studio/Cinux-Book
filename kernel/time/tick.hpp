/**
 * @file    tick.hpp
 * @brief   The kernel's heartbeat service, neutral of every device.
 *
 * Tick counts interrupts and owns the rate knob; a backend owns the
 * hardware. The two meet exactly once, at init: any type satisfying
 * TickSource (compile-time checked) is type-erased into one function
 * pointer plus one context pointer, so consumers see a single stable
 * class no matter which chip drives it — the console-to-serial split one
 * floor up, and the seam a future ARM or LoongArch port swaps without
 * touching a single consumer. Counting and reading are one-liners on
 * purpose: on_interrupt runs inside the IRQ entry, where the shorter the
 * path the better.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_time
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <atomic>
#include <concepts>

#include "cinux/literal_types.hpp"
#include "cinux/singleton.hpp"
#include "kernel/time/tick_config.hpp"

namespace cinux::time {

/**
 * @brief         What a tick backend must offer: start oscillating.
 * @tparam        Backend   The device type being checked.
 * @note          The service is deliberately ignorant of everything
 *                else a timer chip can do; requirements grow the day a
 *                consumer needs them, not before.
 * @since         0.1.0
 * @ingroup       kernel_time
 */
template <typename Backend>
concept TickSource = requires(Backend& backend, cinux::base::Hertz rate) {
    { backend.start(rate) } -> std::same_as<void>;
};

/**
 * @brief         The heartbeat service: counts ticks since boot.
 * @note          Meyers-singleton shape after Pmm; the instance is
 *                constant-initialized, so the zero-construction rule
 *                holds. The class face carries no device name — Pit is
 *                reachable only through the erased pair stored by init.
 * @since         0.1.0
 * @ingroup       kernel_time
 */
class Tick : public cinux::base::Singleton<Tick> {
    friend class cinux::base::Singleton<Tick>;

public:
    /**
     * @brief         Registers a backend and starts the heartbeat.
     *
     * @param[in]     backend   Device to drive, any TickSource type.
     * @return        None
     * @note          Type erasure in full: a captureless lambda becomes
     *                the stored starter function, the backend pointer
     *                rides along as its context. Registering resets the
     *                counter and starts the device at kTickHz at once —
     *                an early-running device is harmless while the
     *                interrupt gates are still closed.
     * @since         0.1.0
     * @ingroup       kernel_time
     */
    template <TickSource Backend>
    void init(Backend& backend) {
        start_ = [](void* backend_ptr, cinux::base::Hertz rate) {
            static_cast<Backend*>(backend_ptr)->start(rate);
        };
        source_ = &backend;
        ticks_.store(0, std::memory_order_relaxed);
        start_(source_, kTickHz);
    }

    /**
     * @brief         Records one heartbeat; called from the IRQ entry.
     *
     * @return        None
     * @note          Inline so it instantiates inside the no-SSE
     *                interrupt island of its caller; the gate has
     *                interrupts off on entry. Atomic access makes the
     *                update visible to polling code even when self()
     *                and since_boot() are inlined; relaxed ordering is
     *                enough because this counter publishes no other data.
     * @since         0.1.0
     * @ingroup       kernel_time
     */
    void on_interrupt() { ticks_.fetch_add(1, std::memory_order_relaxed); }

    /**
     * @brief         Heartbeats counted since init.
     *
     * @return        Tick count; zero before the service starts.
     * @since         0.1.0
     * @ingroup       kernel_time
     */
    [[nodiscard]] unsigned long long since_boot() const {
        return ticks_.load(std::memory_order_relaxed);
    }

private:
    Tick() = default;

    static_assert(std::atomic<unsigned long long>::is_always_lock_free,
                  "The IRQ counter must not call a locking runtime");

    void (*start_)(void*, cinux::base::Hertz) = nullptr;
    void*                           source_   = nullptr;
    std::atomic<unsigned long long> ticks_    = 0;
};

}  // namespace cinux::time
