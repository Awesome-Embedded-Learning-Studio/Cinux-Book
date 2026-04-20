/**
 * @file kernel/drivers/video/font.hpp
 * @brief PSF2 font parser and character renderer
 *
 * Parses a PSF2 font embedded in the kernel binary (via .incbin)
 * and renders individual characters to a Framebuffer.
 *
 * The font data is accessed through symbols defined in font_data.S:
 *   font_psf_start, font_psf_end, font_psf_size
 *
 * Usage:
 *   PSFFont font;
 *   font.init();
 *   font.render_char(fb, 'A', 0, 0, 0x00FFFFFF, 0x00000000);
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers {

class Framebuffer;

class PSFFont {
public:
    /**
     * @brief Initialise the font by parsing the embedded PSF2 data
     *
     * Reads the PSF2 header and stores glyph dimensions for later
     * rendering.  Must be called before render_char().
     */
    void init();

    /**
     * @brief Render a single character to the framebuffer
     *
     * @param fb   Target framebuffer
     * @param c    Character code (0-255)
     * @param x    Top-left X pixel coordinate
     * @param y    Top-left Y pixel coordinate
     * @param fg   Foreground colour (0x00RRGGBB)
     * @param bg   Background colour (0x00RRGGBB)
     */
    void render_char(Framebuffer& fb, uint8_t c, uint32_t x, uint32_t y,
                     uint32_t fg, uint32_t bg);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    const uint8_t* glyphs_ = nullptr;
    uint32_t bytes_per_glyph_ = 0;
    uint32_t num_glyphs_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

}  // namespace cinux::drivers
