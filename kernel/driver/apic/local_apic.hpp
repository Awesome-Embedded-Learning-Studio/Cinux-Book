/**
 * @file local_apic.hpp
 * @brief The xAPIC local controller behind its uncached device window.
 * @author Charliechen114514
 * @date 2026-10-20
 * @version 0.1
 * @since 0.3.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <stdint.h>

#include "cinux/singleton.hpp"

namespace cinux::driver {

/**
 * @brief Local controller operations act on the executing processor.
 * @since 0.3.0
 * @ingroup kernel_driver
 */
class LocalApic : public base::Singleton<LocalApic> {
    friend class base::Singleton<LocalApic>;

public:
    /**
     * @brief Map the firmware address after checking IA32_APIC_BASE.
     * @param[in] physical MADT local controller address.
     * @return True when the address matches an enabled xAPIC controller.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] bool init(uint64_t physical);

    /**
     * @brief Read this processor's physical APIC identifier.
     * @return Eight bit destination identifier.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] uint8_t id() const;

    /**
     * @brief Enable local interrupt delivery with a spurious vector.
     * @param[in] vector Installed spurious interrupt gate.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    void enable(uint8_t vector);

    /**
     * @brief Complete the highest priority local interrupt in service.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    void eoi();

    /**
     * @brief Send a startup or fixed delivery command to one processor.
     * @param[in] destination Physical APIC identifier.
     * @param[in] command Raw ICR low command word.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    void send(uint8_t destination, uint32_t command);

    /**
     * @brief Arm the divide-by-sixteen local timer.
     * @param[in] vector Interrupt vector.
     * @param[in] count Initial down-counter value.
     * @param[in] periodic Repeat each period when true.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    void start_timer(uint8_t vector, uint32_t count, bool periodic);

    /**
     * @brief Sample the local timer down-counter.
     * @return Remaining ticks.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] uint32_t timer_remaining() const;

    /**
     * @brief Stop and mask the local timer.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    void stop_timer();

private:
    LocalApic() = default;
    [[nodiscard]] uint32_t read(unsigned int offset) const;
    void                   write(unsigned int offset, uint32_t value);

    volatile uint32_t* registers_ = nullptr;
};

}
