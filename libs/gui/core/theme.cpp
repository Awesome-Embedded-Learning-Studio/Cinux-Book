/**
 * @file core/theme.cpp
 * @brief cinux::gui Material theme -- assembles the Theme struct from the
 *        centralized colour constants in colors.hpp (P3-b).
 *
 * Compile condition: CINUX_GUI.
 */

#include "colors.hpp"
#include "theme.hpp"

namespace cinux::gui {

Theme material_light() {
    return Theme{colors::light::kPrimary,          colors::light::kOnPrimary,
                 colors::light::kPrimaryContainer, colors::light::kOnPrimaryContainer,
                 colors::light::kSurface,          colors::light::kOnSurface,
                 colors::light::kSurfaceVariant,   colors::light::kBackground,
                 colors::light::kError,            colors::light::kOutline,
                 kRadiusMedium, kRadiusLarge};
}

Theme material_dark() {
    return Theme{colors::dark::kPrimary,          colors::dark::kOnPrimary,
                 colors::dark::kPrimaryContainer, colors::dark::kOnPrimaryContainer,
                 colors::dark::kSurface,          colors::dark::kOnSurface,
                 colors::dark::kSurfaceVariant,   colors::dark::kBackground,
                 colors::dark::kError,            colors::dark::kOutline,
                 kRadiusMedium, kRadiusLarge};
}

}  // namespace cinux::gui
