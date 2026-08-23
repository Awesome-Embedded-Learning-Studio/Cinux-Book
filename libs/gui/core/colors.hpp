/**
 * @file core/colors.hpp
 * @brief Centralized XRGB8888 (0x00RRGGBB) colour constants -- single source
 *        for the ANSI palette + Material light/dark theme colours.
 */
#pragma once
#include <stdint.h>
namespace cinux::gui::colors {

inline constexpr uint32_t kAnsiPalette[16] = {
    0x00000000u, 0x00800000u, 0x00008000u, 0x00808000u,  // black red green yellow
    0x005FADFFu, 0x00800080u, 0x00008080u, 0x00C0C0C0u,  // blue magenta cyan white
    0x00808080u, 0x00FF0000u, 0x0000FF00u, 0x00FFFF00u,  // bright black/red/green/yellow
    0x005FADFFu, 0x00FF00FFu, 0x0000FFFFu, 0x00FFFFFFu,  // bright blue/magenta/cyan/white
};

namespace light {
inline constexpr uint32_t kPrimary            = 0x006200EEu;
inline constexpr uint32_t kOnPrimary          = 0x00FFFFFFu;
inline constexpr uint32_t kPrimaryContainer   = 0x00EADDFFu;
inline constexpr uint32_t kOnPrimaryContainer = 0x0021005Du;
inline constexpr uint32_t kSurface            = 0x00FFFFFFu;
inline constexpr uint32_t kOnSurface          = 0x001A1A1Au;
inline constexpr uint32_t kSurfaceVariant     = 0x00E7E0ECu;
inline constexpr uint32_t kBackground         = 0x00F5F5F5u;
inline constexpr uint32_t kError              = 0x00B00020u;
inline constexpr uint32_t kOutline            = 0x00BDBDBDu;
}  // namespace light

namespace dark {
inline constexpr uint32_t kPrimary            = 0x00BB86FCu;
inline constexpr uint32_t kOnPrimary          = 0x00000000u;
inline constexpr uint32_t kPrimaryContainer   = 0x004F378Bu;
inline constexpr uint32_t kOnPrimaryContainer = 0x00EADDFFu;
inline constexpr uint32_t kSurface            = 0x00121212u;
inline constexpr uint32_t kOnSurface          = 0x00FFFFFFu;
inline constexpr uint32_t kSurfaceVariant     = 0x0049454Fu;
inline constexpr uint32_t kBackground         = 0x002A1C12u;
inline constexpr uint32_t kError              = 0x00CF6679u;
inline constexpr uint32_t kOutline            = 0x00424242u;
}  // namespace dark

}  // namespace cinux::gui::colors
