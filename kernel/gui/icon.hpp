/**
 * @file kernel/gui/icon.hpp
 * @brief Public interface for 32x32 desktop application icons
 *
 * Provides the icon constants (ICON_SIZE, ICON_PIXELS) and re-exports
 * the constexpr pixel data arrays defined in data/icon_data.hpp.
 *
 * Consumers should include this header and use:
 *   cinux::gui::icons::ICON_SIZE
 *   cinux::gui::icons::k_shell_icon
 *   cinux::gui::icons::k_calc_icon
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui::icons
 */

#pragma once

#include <cstdint>

namespace cinux::gui::icons {

/// Standard icon dimensions (width == height)
constexpr uint32_t ICON_SIZE = 32;

/// Number of pixels per icon (ICON_SIZE x ICON_SIZE)
constexpr uint32_t ICON_PIXELS = ICON_SIZE * ICON_SIZE;

}  // namespace cinux::gui::icons

// Pull in the actual pixel data arrays
#include "kernel/gui/data/icon_data.hpp"
