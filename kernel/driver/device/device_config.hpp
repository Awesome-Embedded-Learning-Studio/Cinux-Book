/**
 * @file    device_config.hpp
 * @brief   Capacity and shape words for the device model.
 *
 * One home for the numbers the device model's tables are sized by: how
 * many devices the whole model tracks and how long a bus or driver
 * label may run. Facts about a family of silicon — page sizes, frame
 * lengths — do not live here; each family carries its own config.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::driver {

/**
 * @brief   How many devices the whole model tracks at once.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kDeviceModelMax = 16;

/**
 * @brief   How long a bus or driver label may run, NUL included.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kDeviceLabelMax = 12;

}  // namespace cinux::driver
