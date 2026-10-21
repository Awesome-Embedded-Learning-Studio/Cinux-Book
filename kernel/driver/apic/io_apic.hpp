/**
 * @file io_apic.hpp
 * @brief ISA delivery through the first firmware IOAPIC.
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
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/interrupt/irq_config.hpp"

namespace cinux::interrupt {
struct IrqLine;
}

namespace cinux::driver {

/**
 * @brief Indirect IOAPIC register access and the IrqChip backend.
 * @since 0.3.0
 * @ingroup kernel_driver
 */
class IoApic : public base::Singleton<IoApic> {
    friend class base::Singleton<IoApic>;

public:
    /**
     * @brief Map and mask the controller, retaining firmware routes.
     * @param[in] topology Decoded MADT.
     * @return True when every ISA route fits the controller.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    [[nodiscard]] bool init(const acpi::InterruptTopology& topology);

    /**
     * @brief Route and open one ISA line to the boot processor.
     * @param[in] line ISA source before firmware overrides.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    void unmask(interrupt::IrqLine line);

    /**
     * @brief Acknowledge local delivery of an IOAPIC interrupt.
     * @param[in] line Service line being completed.
     * @since 0.3.0
     * @ingroup kernel_driver
     */
    void ack(interrupt::IrqLine line);

private:
    IoApic() = default;
    [[nodiscard]] uint32_t read(unsigned int index);
    void                   write(unsigned int index, uint32_t value);

    volatile uint32_t* registers_                        = nullptr;
    uint32_t           gsi_base_                         = 0;
    uint32_t           pin_count_                        = 0;
    uint8_t            destination_                      = 0;
    acpi::IrqOverride  routes_[interrupt::kIrqLineCount] = {};
};

}
