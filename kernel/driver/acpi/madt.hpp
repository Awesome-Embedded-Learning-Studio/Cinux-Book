/**
 * @file madt.hpp
 * @brief Decoded interrupt topology and firmware lookup.
 * @author Charliechen114514
 * @date 2026-10-20
 * @version 0.1
 * @since 0.3.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <stdint.h>

#include "kernel/driver/acpi/madt_config.hpp"

namespace cinux::driver::acpi {

struct Rsdp;

/**
 * @brief ISA source routing supplied by firmware.
 * @since 0.3.0
 * @ingroup kernel_driver
 */
struct IrqOverride {
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
};

/**
 * @brief Enabled processors and the first IOAPIC with its ISA overrides.
 * @since 0.3.0
 * @ingroup kernel_driver
 */
struct InterruptTopology {
    uint64_t     local_apic_address                     = 0;
    uint32_t     io_apic_address                        = 0;
    uint32_t     io_apic_gsi_base                       = 0;
    bool         pcat_compatible                        = false;
    uint8_t      processor_apic_ids[kMaxProcessorApics] = {};
    unsigned int processor_count                        = 0;
    IrqOverride  overrides[kMaxIrqOverrides]            = {};
    unsigned int override_count                         = 0;
};

/**
 * @brief Decode a bounded MADT byte image.
 * @param[in] table Table bytes, possibly unaligned.
 * @param[in] bytes Available image size.
 * @return Empty topology for an absent, truncated or malformed table.
 * @since 0.3.0
 * @ingroup kernel_driver
 */
[[nodiscard]] InterruptTopology ParseMadt(const unsigned char* table, unsigned long bytes);

/**
 * @brief Find and decode the MADT through the shared ACPI table walk.
 * @param[in] root Firmware root pointer.
 * @return Decoded topology, empty when absent.
 * @since 0.3.0
 * @ingroup kernel_driver
 */
[[nodiscard]] InterruptTopology FindInterruptTopology(const Rsdp* root);

}
