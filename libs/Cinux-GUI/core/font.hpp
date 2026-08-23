/**
 * @file core/font.hpp
 * @brief PSF2 font parser -- exposes glyph bitmaps for swraster::glyph_blit
 *
 * Parses the constexpr PSF2 blob (core/font_psf_data.hpp) and indexes glyphs.
 * Each glyph is width()*height() pixels packed MSB-first, one byte per row
 * (width <= 8); this matches swraster::glyph_blit's (bits, gw, gh) contract
 * directly -- hand glyph(c) + width()/height() straight to glyph_blit.
 *
 * Host-neutral: pure integer parse over a constexpr array, no file I/O.
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

namespace cinux::gui {

/**
 * @brief PSF2 console font -- parse once, index glyphs for rasterisation
 *
 * init() parses the header; glyph(c) returns the bitmap for character @p c
 * (height() bytes, MSB-first). Out-of-range codes collapse to glyph 0, matching
 * the Cinux kernel renderer. Not copyable (stateless beyond parsed views, but
 * kept non-copyable for clarity -- it borrows the constexpr blob).
 */
class PsfFont {
public:
    /** Parse the PSF2 header from kFontPsfData. No-op (stays !ready) on bad magic. */
    void init();

    /**
     * @brief Glyph bitmap for @p c (height() bytes, MSB-first per row)
     *
     * @p c >= num_glyphs() collapses to glyph 0. Returns nullptr before init()
     * or on a malformed blob.
     */
    const uint8_t* glyph(uint8_t c) const;

    /** Glyph width in pixels (8 for the bundled font). */
    uint32_t width() const { return width_; }

    /** Glyph height in pixels (16 for the bundled font). */
    uint32_t height() const { return height_; }

    /** Number of glyphs in the font (256 for the bundled font). */
    uint32_t num_glyphs() const { return num_glyphs_; }

    /** Bytes per glyph (rows * ceil(width/8)). */
    uint32_t bytes_per_glyph() const { return bytes_per_glyph_; }

    /** True after a successful init() -- glyph() is usable. */
    bool ready() const { return glyphs_ != nullptr; }

private:
    const uint8_t* glyphs_          = nullptr;
    uint32_t       num_glyphs_      = 0;
    uint32_t       bytes_per_glyph_ = 0;
    uint32_t       width_           = 0;
    uint32_t       height_          = 0;
};

/** Width of @p str rendered at integer @p scale (longest line; 0 if !ready/null). P5-a. */
uint32_t text_width(const PsfFont& f, const char* str, uint32_t scale);

/** Height of @p str at @p scale (line count * font.height() * scale). P5-a. */
uint32_t text_height(const PsfFont& f, const char* str, uint32_t scale);

}  // namespace cinux::gui
