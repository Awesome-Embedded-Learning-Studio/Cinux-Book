/**
 * @file madt_config.hpp
 * @brief Bounds of the retained interrupt topology.
 * @author Charliechen114514
 * @date 2026-10-20
 * @version 0.1
 * @since 0.3.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */
#pragma once

namespace cinux::driver::acpi {

inline constexpr unsigned int kMaxProcessorApics = 8;
inline constexpr unsigned int kMaxIrqOverrides   = 16;

}
