/**
 * @file core/icon_data.hpp
 * @brief Constexpr 32x32 pixel data + 1-bpp alpha masks for desktop icons
 *
 * Each icon is authored as 32 row-strings of hex nibbles (one char per pixel)
 * mapped through a 16-entry palette. Two artefacts are generated at compile
 * time from the SAME row-strings:
 *   - IconBitmap : the 1024 uint32_t colour pixels (0x00RRGGBB, row-major).
 *   - IconMask   : a 1-bpp alpha mask (32 rows x 4 bytes, MSB-first per row).
 *
 * Transparency is governed by the MASK, not by the colour value. A mask bit is
 * set iff the source nibble is non-zero (the authored "nibble 0 = transparent"
 * convention) -- independent of what colour that nibble maps to.
 *
 * Palette: amber/warm (F13-B). The icon SHAPES are lifted verbatim from the
 * legacy CinuxOS desktop (pre-Cinux-GUI migration); only the palette colours
 * were retuned to a warm amber tone so the new desktop is visually distinct
 * from the old dark-teal + green-terminal look, without redrawing art.
 *
 * Pure C++17 (stdint only), ZERO host includes.
 *
 * Namespace: cinux::gui::icons::data
 */
#pragma once

#include <cstdint>

namespace cinux::gui::icons::data {

// ============================================================
// Colour palette -- amber/warm (F13-B desktop)
// ============================================================

namespace palette {
constexpr uint32_t BLACK        = 0x00000000;  // Transparent slot (mask-driven)
constexpr uint32_t AMBER        = 0x00FFB000;  // Primary amber (cursor / equals)
constexpr uint32_t AMBER_LIGHT  = 0x00FFD878;  // Light amber (LCD display bg)
constexpr uint32_t WARM_WHITE   = 0x00FFF0E0;  // Warm white (prompt text)
constexpr uint32_t BROWN_DARK   = 0x002A1F0A;  // Deep brown (terminal body)
constexpr uint32_t BROWN_MID    = 0x004A3520;  // Mid brown (title bar / borders)
constexpr uint32_t BEIGE        = 0x00C8A878;  // Warm beige (calculator body)
constexpr uint32_t BEIGE_DARK   = 0x008A7050;  // Dark beige (buttons)
constexpr uint32_t BEIGE_LIGHT  = 0x00E0C898;  // Light beige (button highlight)
}  // namespace palette

// ============================================================
// Compile-time icon builder
// ============================================================

namespace detail {

/// Fixed 1024-pixel (32x32) bitmap. Freestanding aggregate (no STL).
struct IconBitmap {
    uint32_t pixels[1024];

    constexpr uint32_t&       operator[](uint32_t i) { return pixels[i]; }
    constexpr const uint32_t& operator[](uint32_t i) const { return pixels[i]; }
    constexpr const uint32_t* data() const { return pixels; }
};

/// Map a single hex character to a 4-bit nibble (0-15).
constexpr uint32_t hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return static_cast<uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<uint32_t>(c - 'a') + 10;
    if (c >= 'A' && c <= 'F')
        return static_cast<uint32_t>(c - 'A') + 10;
    return 0;
}

/// Look up a palette colour by nibble index.
constexpr uint32_t palette_lookup(const uint32_t (&pal)[16], uint32_t nibble) {
    return pal[nibble];
}

/**
 * @brief Build a full 32x32 icon from 32 row hex strings and a palette
 *
 * Each row string must be exactly 32 characters; characters are mapped through
 * the palette to produce 32-bit pixel values.
 */
template <uint32_t Rows>
constexpr IconBitmap build_icon(const uint32_t (&palette)[16], const char* const (&rows)[Rows]) {
    static_assert(Rows == 32, "Icon must have exactly 32 rows");

    IconBitmap pixels{};
    for (uint32_t r = 0; r < 32; r++) {
        for (uint32_t c = 0; c < 32; c++) {
            uint32_t nibble    = hex_nibble(rows[r][c]);
            pixels[r * 32 + c] = palette_lookup(palette, nibble);
        }
    }
    return pixels;
}

/// Fixed 32x32 1-bpp alpha mask (32 rows x 4 bytes). Set bit = opaque pixel.
struct IconMask {
    uint8_t bytes[128];

    constexpr uint8_t&       operator[](uint32_t i) { return bytes[i]; }
    constexpr const uint8_t* data() const { return bytes; }
};

/**
 * @brief Build a 1-bpp alpha mask from the same 32 row-strings as build_icon
 *
 * A mask bit is set iff the source nibble is non-zero (nibble 0 is the
 * authored "transparent" slot), independent of the palette colour.
 */
template <uint32_t Rows>
constexpr IconMask build_mask(const char* const (&rows)[Rows]) {
    static_assert(Rows == 32, "Icon mask must have exactly 32 rows");

    IconMask m{};
    for (uint32_t r = 0; r < 32; r++) {
        for (uint32_t c = 0; c < 32; c++) {
            if (hex_nibble(rows[r][c]) != 0) {
                const uint32_t byte_idx = r * 4u + c / 8u;
                const uint32_t bit_idx  = 7u - (c % 8u); /* MSB-first per row */
                m.bytes[byte_idx] |= static_cast<uint8_t>(1u << bit_idx);
            }
        }
    }
    return m;
}

}  // namespace detail

// ============================================================
// Shell icon -- deep-brown terminal with amber ">_" prompt
// ============================================================

/**
 * @brief Shell/terminal icon palette (amber/warm)
 *
 * Digit mapping (shape lifted from the legacy desktop; colours retuned):
 *   0 = transparent   1 = BROWN_DARK (body)
 *   2 = BROWN_MID (title bar)  3 = WARM_WHITE (prompt text)
 *   4 = AMBER (cursor)  5/6/7 = traffic-light dots (kept: red/yellow/green)
 */
inline constexpr uint32_t k_shell_palette[16] = {
    palette::BLACK,       // 0 - transparent
    palette::BROWN_DARK,  // 1 - terminal body (was near-black)
    palette::BROWN_MID,   // 2 - title bar (was dark grey)
    palette::WARM_WHITE,  // 3 - prompt text (was white)
    palette::AMBER,       // 4 - cursor (was terminal green) -- AMBER
    0x00CC3333,           // 5 - close dot red (traffic light, kept)
    0x00CCCC33,           // 6 - minimise dot yellow (kept)
    0x0033CC33,           // 7 - maximise dot green (kept)
};

/**
 * @brief 32x32 shell icon -- authored row-strings (shape from legacy desktop)
 *
 * Visual: deep-brown rounded terminal body, three traffic-light dots in the
 * title bar, warm-white ">_" prompt with an amber cursor block.
 */
inline constexpr const char* const k_shell_rows[32] = {
    "00222222222222222222222222222220", "02255672222222222222222222222220",
    "02222222222222222222222222222220", "02111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01331111111111111111111111111110",
    "01134111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "01111111111111111111111111111110",
    "01111111111111111111111111111110", "00222222222222222222222222222220",
};

/// Shell icon colour pixels (generated from k_shell_rows).
inline constexpr auto k_shell_icon = detail::build_icon(k_shell_palette, k_shell_rows);

/// Shell icon 1-bpp alpha mask (generated from k_shell_rows).
inline constexpr auto k_shell_mask = detail::build_mask(k_shell_rows);

// ============================================================
// Calculator icon -- beige body with amber LCD and button grid
// ============================================================

/**
 * @brief Calculator icon palette (amber/warm)
 *
 * Digit mapping (shape lifted from the legacy desktop; colours retuned):
 *   0 = transparent   1 = BEIGE (body)
 *   2 = BROWN_MID (border/grid)  3 = BEIGE_DARK (buttons)
 *   4 = AMBER_LIGHT (display bg)  5 = BROWN_DARK (display text)
 *   6 = AMBER (equals)  7 = BEIGE_LIGHT (button highlight)
 */
inline constexpr uint32_t k_calc_palette[16] = {
    palette::BLACK,        // 0 - transparent
    palette::BEIGE,        // 1 - body (was mid grey)
    palette::BROWN_MID,    // 2 - border / grid lines (was dark grey)
    palette::BEIGE_DARK,   // 3 - buttons (was button grey)
    palette::AMBER_LIGHT,  // 4 - display background (was greenish LCD) -- AMBER
    palette::BROWN_DARK,   // 5 - display text (was dark grey)
    palette::AMBER,        // 6 - equals button (was orange) -- AMBER
    palette::BEIGE_LIGHT,  // 7 - button highlight (was light grey)
};

/**
 * @brief 32x32 calculator icon -- authored row-strings (shape from legacy desktop)
 *
 * Visual: rounded beige body, amber LCD showing "123", 4-column button grid
 * with an amber equals key.
 */
inline constexpr const char* const k_calc_rows[32] = {
    "00222222222222222222222222222220", "02111111111111111111111111111120",
    "01244444444444444444444444444110", "01245551111111111111111111111110",
    "01244444444444444444444444444110", "01222222222222222222222222222210",
    "01273273273273273273273273273210", "01233233233233233233233233233210",
    "01273273273273273273273273273210", "01233233233233233233233233233210",
    "01273273273273273273273273273210", "01233233233233233233233233233210",
    "01273273273273273273273273273210", "01233233233233233233233233233210",
    "01273333333333333333333333673210", "01233333333333333333333333363210",
    "01222222222222222222222222222210", "02111111111111111111111111111120",
    "00222222222222222222222222222220", "00000000000000000000000000000000",
    "00000000000000000000000000000000", "00000000000000000000000000000000",
    "00000000000000000000000000000000", "00000000000000000000000000000000",
    "00000000000000000000000000000000", "00000000000000000000000000000000",
    "00000000000000000000000000000000", "00000000000000000000000000000000",
    "00000000000000000000000000000000", "00000000000000000000000000000000",
    "00000000000000000000000000000000", "00000000000000000000000000000000",
};

/// Calculator icon colour pixels (generated from k_calc_rows).
inline constexpr auto k_calc_icon = detail::build_icon(k_calc_palette, k_calc_rows);

/// Calculator icon 1-bpp alpha mask (generated from k_calc_rows).
inline constexpr auto k_calc_mask = detail::build_mask(k_calc_rows);

}  // namespace cinux::gui::icons::data
