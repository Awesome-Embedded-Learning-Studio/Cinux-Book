/**
 * @file core/theme.hpp
 * @brief cinux::gui Material theme -- flat colour palette + shape defaults (P3-b)
 *
 * The flat-Material subset (P3 decision B): solid colours + corner radius + an
 * 8px grid. NO blur shadows, ripple, or motion -- those need an animation
 * system (P5+). Elevation is implied by picking a surface variant colour, not
 * by a computed shadow. Pure data; widgets read a Theme to pick colours.
 *
 * Pure C++17 (stdint only), ZERO host includes.
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

namespace cinux::gui {

/* 8-pixel grid. We render at fixed pixels, so "dp" == px here. */
inline constexpr uint32_t kGridUnit  = 8;
inline constexpr uint32_t kSpacing4  = 4;
inline constexpr uint32_t kSpacing8  = 8;
inline constexpr uint32_t kSpacing16 = 16;

/* Corner radii (Material shape scale). */
inline constexpr uint32_t kRadiusSmall  = 4;
inline constexpr uint32_t kRadiusMedium = 8;
inline constexpr uint32_t kRadiusLarge  = 16;

/**
 * @brief Material colour palette + shape defaults
 *
 * Colours are XRGB8888 (0x00RRGGBB). Widgets pick from these via the Theme
 * rather than hardcoding, so light/dark swap is one call.
 */
struct Theme {
    uint32_t primary;               // accent / filled buttons
    uint32_t on_primary;            // text/icon on primary
    uint32_t primary_container;     // P5-b: tonal container (secondary fill)
    uint32_t on_primary_container;  // text on primary_container
    uint32_t surface;               // card / button face
    uint32_t on_surface;            // text on surface
    uint32_t surface_variant;       // P5-b: muted surface (subtle bg/borders)
    uint32_t background;            // app background
    uint32_t error;                 // error
    uint32_t outline;               // hairline borders
    uint32_t button_radius;         // default button corner radius (px)
    uint32_t card_radius;           // default card corner radius (px)
};

/** Material light theme (the default). */
Theme material_light();

/** Material dark theme. */
Theme material_dark();

}  // namespace cinux::gui
