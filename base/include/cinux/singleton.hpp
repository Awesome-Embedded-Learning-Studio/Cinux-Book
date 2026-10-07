/**
 * @file    singleton.hpp
 * @brief   The one Meyers-singleton shell, inherited everywhere.
 *
 * Every kernel service that owns exactly one instance wore its own
 * hand-rolled self() — same five lines, ten files, no shared face.
 * This shell is those five lines, once: a CRTP base whose self()
 * houses a constinit function-local static and returns it as the
 * derived type. Derive, befriend the shell, keep your constructor
 * private: instantiation stays a family secret while every call
 * site keeps reading Type::self() exactly as before.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_utility
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <type_traits>

namespace cinux::base {

/**
 * @brief         Inherit to become the one instance of your type.
 * @tparam        Instance   The deriving type (CRTP).
 * @note          The base constructor is private and befriends Instance;
 *                the derived type befriends this shell so the hidden
 *                static can call its private constructor. Copies and
 *                moves are deleted with the base.
 * @since         0.1.0
 * @ingroup       base_utility
 */
template <typename Instance>
class Singleton {
public:
    Singleton(const Singleton&)            = delete;
    Singleton& operator=(const Singleton&) = delete;

    /**
     * @brief         The one instance, initialized before execution.
     * @return        Reference to the shared instance.
     * @note          constinit rejects dynamic initialization; a trivial
     *                destructor avoids runtime exit registration. The
     *                typed object preserves its lifetime and any constant
     *                member initializers. Zero initializers use .bss;
     *                nonzero constant initializers can use .data. Shared
     *                state still needs its own interrupt/concurrency
     *                discipline when self() is inlined.
     * @since         0.1.0
     * @ingroup       base_utility
     */
    static Instance& self() {
        static_assert(std::is_trivially_destructible_v<Instance>,
                      "Singleton instances must not require runtime destruction");
        static constinit Instance instance{};
        return instance;
    }

private:
    friend Instance;

    Singleton() = default;
};

}  // namespace cinux::base
