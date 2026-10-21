/**
 * @file spin_mutex.hpp
 * @brief OS-independent atomic exclusion for short non-sleeping sections.
 * @author Charliechen114514
 * @date 2026-10-20
 * @version 0.1
 * @since 0.3.0
 * @ingroup base_utility
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <atomic>

#include "cinux/scoped_guard.hpp"

namespace cinux::base {

/**
 * @brief Acquire/release exclusion with interrupt policy left to its caller.
 * @since 0.3.0
 * @ingroup base_utility
 */
class SpinMutex {
    /** @brief Mutex acquisition policy for the shared guard. */
    struct LockPolicy {
        /** @brief Mutex whose ownership follows the scope. */
        struct State {
            SpinMutex& mutex;
        };
        static void enter(State& state) { state.mutex.lock(); }
        /** @brief Release the mutex acquired by enter. */
        static void exit(State& state) { state.mutex.unlock(); }
    };

public:
    /**
     * @brief Hold one mutex through the shared scope-guard shell.
     * @since 0.3.0
     * @ingroup base_utility
     */
    using Guard = ScopedGuard<LockPolicy>;

    /**
     * @brief Wait until exclusive ownership is acquired.
     * @since 0.3.0
     * @ingroup base_utility
     */
    void lock() {
        while (held_.test_and_set(std::memory_order_acquire)) {
            while (held_.test(std::memory_order_relaxed)) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
        }
    }

    /**
     * @brief Publish changes and release exclusive ownership.
     * @since 0.3.0
     * @ingroup base_utility
     */
    void unlock() { held_.clear(std::memory_order_release); }

    /**
     * @brief Acquire for the returned guard's lifetime.
     * @return An owning scope guard.
     * @since 0.3.0
     * @ingroup base_utility
     */
    [[nodiscard]] Guard guard() { return Guard(*this); }

private:
    std::atomic_flag held_;
};

}
