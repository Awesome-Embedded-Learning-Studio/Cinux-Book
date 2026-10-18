/**
 * @file    hpet.hpp
 * @brief   The High Precision Event Timer as a free-running clock face.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>

#include "cinux/singleton.hpp"
#include "kernel/driver/acpi/acpi.hpp"

namespace cinux::driver {

/**
 * @brief   The HPET counter, asked from ACPI and read through MMIO.
 * @note    A clock-only slice: the counter register at block base plus
 *          0xF0 counts up at the period the capability register
 *          publishes in femtoseconds. Init enables the main counter
 *          with legacy IRQ routing off. Nothing here arms comparators
 *          or interrupts — the PIT keeps the heartbeat; this is the
 *          fine hand the clock syscalls read between ticks.
 * @since   0.3.0
 * @ingroup kernel_driver
 */
class Hpet : public cinux::base::Singleton<Hpet> {
    friend class cinux::base::Singleton<Hpet>;

public:
    /**
     * @brief     Finds the timer in the ACPI tree and remembers it.
     *
     * @param[in] root   The root pointer ACPI handed over.
     * @return    True when a system-memory block with a valid period
     *            and a 64-bit counter was mapped and enabled.
     * @note      Address spaces other than system memory, or a period
     *            the capability register refuses to name, leave the singleton inert;
     *            every later face then answers not-present.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    bool init(const acpi::Rsdp* root);

    /**
     * @brief     Nanoseconds since the timer was first found.
     *
     * @return    The count scaled to nanoseconds, zero when inert.
     * @note      The raw counter is scaled by its femtoseconds-per-tick
     *            period; the boot-relative base is taken at init, so
     *            two readings subtract cleanly however late init ran.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] unsigned long long since_boot_ns() const;

    /// Whether init found a usable timer.
    [[nodiscard]] bool present() const { return counter_ != nullptr; }

private:
    Hpet() = default;

    volatile uint64_t* counter_    = nullptr;
    unsigned long long period_fs_  = 0;
    unsigned long long boot_count_ = 0;
};

}  // namespace cinux::driver
