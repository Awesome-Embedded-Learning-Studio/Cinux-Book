/**
 * @file    font.hpp
 * @brief   PSF2 bitmap font grammar: parse once, ask pixels forever.
 *
 * The parser is pure constexpr math over a byte blob so the host world can
 * grow sentinels over synthetic fonts, while the kernel feeds it the
 * embedded asset. Width is capped at eight pixels: one glyph row is one
 * byte, MSB leftmost, which is the whole reason the grammar stays small.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/byte_order.hpp"

namespace cinux::console {

/// Magic every PSF2 file begins with, little-endian on disk.
inline constexpr uint32_t kPsf2Magic = 0x864AB572;

/// Bytes in a PSF2 header: eight 32-bit fields.
inline constexpr uint32_t kPsf2HeaderBytes = 32;

/// Widest glyph this grammar understands: one row must fit one byte.
inline constexpr uint32_t kPsf2MaxWidth = 8;

/// The facts a renderer needs from one PSF2 font blob.
struct Psf2Font {
    /// Whether the blob parsed as a usable font; everything below is
    /// garbage when false and callers must not render.
    bool valid;

    /// How many glyphs the font carries.
    uint32_t glyph_count;

    /// Bytes per glyph; one byte per pixel row.
    uint32_t charsize;

    /// Pixel rows per glyph.
    uint32_t height;

    /// Pixel columns per glyph, at most kPsf2MaxWidth.
    uint32_t width;
};

/**
 * @brief         The one font the kernel image embeds; defined in font.cpp.
 *
 * @return        Parsed facts of the embedded font.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
struct Psf2Font EmbeddedFont();

/**
 * @brief         First byte of the embedded font blob; pair it with
 *                EmbeddedFont().
 *
 * @return        Pointer to the font blob's first byte.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
const unsigned char* EmbeddedFontData();

/**
 * @brief         Parse one PSF2 font blob into its render facts, refusing
 *                anything truncated, mis-signed, or wider than one byte
 *                per row.
 *
 * @param[in]     data   First byte of the blob.
 * @param[in]     bytes  Size of the blob in bytes.
 * @return        The font facts; valid is false on any refusal.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
constexpr Psf2Font ParsePsf2(const unsigned char* data, uint64_t bytes) {
    Psf2Font font{};
    if (bytes < kPsf2HeaderBytes) {
        return font;
    }
    font.glyph_count = cinux::base::ReadWord32(data, 16);
    font.charsize    = cinux::base::ReadWord32(data, 20);
    font.height      = cinux::base::ReadWord32(data, 24);
    font.width       = cinux::base::ReadWord32(data, 28);
    uint64_t const kBodyBytes =
        static_cast<uint64_t>(font.glyph_count) * static_cast<uint64_t>(font.charsize);
    bool const kShapeOk = font.width != 0 && font.width <= kPsf2MaxWidth && font.height != 0 &&
                          font.charsize >= font.height && font.glyph_count != 0;
    font.valid          = cinux::base::ReadWord32(data, 0) == kPsf2Magic && kShapeOk &&
                          static_cast<uint64_t>(kPsf2HeaderBytes) + kBodyBytes <= bytes;
    if (!font.valid) {
        font = Psf2Font{};
    }
    return font;
}

/**
 * @brief         Fetches one glyph's pixel row as a raw byte, MSB
 *                leftmost.
 *
 * @param[in]     data    The blob the font was parsed from.
 * @param[in]     font    Parsed font facts.
 * @param[in]     glyph   Glyph index; the low byte of the character.
 * @param[in]     row     Pixel row inside the glyph.
 * @return        The row's bits.
 * @warning       Undefined when font.valid is false or the arguments
 *                leave the blob; callers gate on valid and truncate
 *                indices.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
constexpr unsigned char GlyphRowBits(const unsigned char* data, const Psf2Font& font,
                                     uint32_t glyph, uint32_t row) {
    uint64_t const kGlyphOffset =
        static_cast<uint64_t>(kPsf2HeaderBytes) +
        (static_cast<uint64_t>(glyph) * static_cast<uint64_t>(font.charsize)) + row;
    return data[kGlyphOffset];
}

/**
 * @brief         Tests one pixel of a glyph row: column zero is the
 *                leftmost, highest bit.
 *
 * @param[in]     row_bits  The row byte from GlyphRowBits.
 * @param[in]     column    Pixel column inside the glyph.
 * @return        True when the pixel is set.
 * @warning       Undefined for column >= kPsf2MaxWidth.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
constexpr bool GlyphPixel(unsigned char row_bits, uint32_t column) {
    return ((row_bits >> (kPsf2MaxWidth - 1U - column)) & 1U) != 0;
}

}  // namespace cinux::console
