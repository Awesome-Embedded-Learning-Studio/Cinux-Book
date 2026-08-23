/**
 * @file core/font.cpp
 * @brief PSF2 font parser implementation
 *
 * Parses the constexpr PSF2 blob with explicit little-endian byte reads (no
 * packed struct, no reinterpret_cast -- so no aliasing UB and no host-endian
 * assumption). Pure integer arithmetic over kFontPsfData; no file I/O.
 *
 * Compile condition: CINUX_GUI.
 */

#include "font.hpp"

#include <stdint.h>

#include "font_psf_data.hpp"

namespace cinux::gui {
namespace {

/* PSF2 magic, little-endian 0x864AB572. */
constexpr uint32_t kPsf2Magic = 0x864AB572u;

/* PSF2 header field offsets (all 32-bit little-endian). */
constexpr uint32_t kOffMagic      = 0;
constexpr uint32_t kOffNumGlyphs  = 16;
constexpr uint32_t kOffBytesGlyph = 20;
constexpr uint32_t kOffHeight     = 24;
constexpr uint32_t kOffWidth      = 28;
constexpr uint32_t kOffHeaderSize = 8;
constexpr uint32_t kPsf2HeaderLen = 32;

inline uint32_t le_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace

void PsfFont::init() {
    if (kFontPsfSize < kPsf2HeaderLen) {
        return; /* blob too short for a PSF2 header -> stays !ready */
    }
    if (le_u32(kFontPsfData + kOffMagic) != kPsf2Magic) {
        return; /* not PSF2 -> stays !ready */
    }
    const uint32_t header_size = le_u32(kFontPsfData + kOffHeaderSize);
    if (header_size > kFontPsfSize) {
        return; /* malformed -> stays !ready */
    }
    num_glyphs_      = le_u32(kFontPsfData + kOffNumGlyphs);
    bytes_per_glyph_ = le_u32(kFontPsfData + kOffBytesGlyph);
    height_          = le_u32(kFontPsfData + kOffHeight);
    width_           = le_u32(kFontPsfData + kOffWidth);
    glyphs_          = kFontPsfData + header_size;
}

const uint8_t* PsfFont::glyph(uint8_t c) const {
    if (glyphs_ == nullptr) {
        return nullptr;
    }
    if (c >= num_glyphs_) {
        c = 0; /* out-of-range collapses to glyph 0, matching the kernel renderer */
    }
    return glyphs_ + static_cast<uint32_t>(c) * bytes_per_glyph_;
}

uint32_t text_width(const PsfFont& f, const char* str, uint32_t scale) {
    if (!f.ready() || str == nullptr) {
        return 0u;
    }
    /* Longest line width; '\n' ends a line. */
    uint32_t max_w = 0u;
    uint32_t cur   = 0u;
    for (const char* p = str; *p != '\0'; ++p) {
        if (*p == '\n') {
            if (cur > max_w) {
                max_w = cur;
            }
            cur = 0u;
        } else {
            cur += f.width() * scale;
        }
    }
    return (cur > max_w) ? cur : max_w;
}

uint32_t text_height(const PsfFont& f, const char* str, uint32_t scale) {
    if (!f.ready()) {
        return 0u;
    }
    uint32_t lines = 1u;
    if (str != nullptr) {
        for (const char* p = str; *p != '\0'; ++p) {
            if (*p == '\n') {
                ++lines;
            }
        }
    }
    return lines * f.height() * scale;
}

}  // namespace cinux::gui
