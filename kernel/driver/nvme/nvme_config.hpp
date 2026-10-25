/**
 * @file nvme_config.hpp
 * @brief NVMe queue and transfer bounds.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <cstdint>

namespace cinux::driver {
/** @brief Entries in each admin and I/O queue. */
inline constexpr std::uint16_t      kNvmeQueueEntries = 8;
/** @brief Controller and command timeout in nanoseconds. */
inline constexpr unsigned long long kNvmeTimeoutNs    = 2'000'000'000ULL;
/** @brief NVM Express PCI storage subclass. */
inline constexpr std::uint8_t       kNvmeSubclass     = 8;
}  // namespace cinux::driver
