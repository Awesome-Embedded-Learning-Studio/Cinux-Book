/**
 * @file    keyboard_config.hpp
 * @brief   Configuration facts for the PS/2 keyboard driver.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::driver {

/// Key events the keyboard keeps before the consumer drains them; one
/// slot stays empty so full and empty stay distinguishable.
inline constexpr unsigned int kKeyboardEventCapacity = 64;

}  // namespace cinux::driver
