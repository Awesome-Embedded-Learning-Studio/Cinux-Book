#include <stdint.h>

#include "../framework/framework.hpp"
#include "kernel/console/font.hpp"
#include "test_assert.hpp"

using cinux::console::GlyphPixel;
using cinux::console::GlyphRowBits;
using cinux::console::ParsePsf2;
using cinux::console::Psf2Font;

namespace {

constexpr unsigned int kMiniGlyphs   = 2;
constexpr unsigned int kMiniCharsize = 2;
constexpr unsigned int kMiniBytes =
    cinux::console::kPsf2HeaderBytes + (kMiniGlyphs * kMiniCharsize);

struct MiniFont {
    unsigned char bytes[kMiniBytes]{};

    constexpr MiniFont() {
        store(0, cinux::console::kPsf2Magic);
        store(4, 0);
        store(8, cinux::console::kPsf2HeaderBytes);
        store(12, 0);
        store(16, kMiniGlyphs);
        store(20, kMiniCharsize);
        store(24, 2);
        store(28, 8);
        bytes[32] = 0x80;
        bytes[33] = 0x40;
        bytes[34] = 0x20;
        bytes[35] = 0x10;
    }

    constexpr void store(unsigned int field, uint32_t value) {
        for (unsigned int byte_index = 0; byte_index < 4; ++byte_index) {
            bytes[field + byte_index] =
                static_cast<unsigned char>((value >> (8U * byte_index)) & 0xFFU);
        }
    }
};

constexpr MiniFont kGood{};

}  // namespace

TEST("font: mini PSF2 header parses into facts") {
    Psf2Font const kFont = ParsePsf2(kGood.bytes, kMiniBytes);
    ASSERT_TRUE(kFont.valid);
    ASSERT_TRUE(kFont.glyph_count == kMiniGlyphs);
    ASSERT_TRUE(kFont.charsize == kMiniCharsize);
    ASSERT_TRUE(kFont.height == 2);
    ASSERT_TRUE(kFont.width == 8);
}

TEST("font: bad magic is refused, not rendered") {
    MiniFont cursed{};
    cursed.bytes[0]      = 0x00;
    Psf2Font const kFont = ParsePsf2(cursed.bytes, kMiniBytes);
    ASSERT_TRUE(!kFont.valid);
}

TEST("font: truncated header is refused") {
    Psf2Font const kFont = ParsePsf2(kGood.bytes, 16);
    ASSERT_TRUE(!kFont.valid);
}

TEST("font: body shorter than glyph table is refused") {
    Psf2Font const kFont = ParsePsf2(kGood.bytes, kMiniBytes - 1);
    ASSERT_TRUE(!kFont.valid);
}

TEST("font: width beyond one byte per row is refused") {
    MiniFont wide{};
    wide.store(28, 9);
    Psf2Font const kFont = ParsePsf2(wide.bytes, kMiniBytes);
    ASSERT_TRUE(!kFont.valid);
}

TEST("font: charsize below height is refused") {
    MiniFont thin{};
    thin.store(20, 1);
    Psf2Font const kFont = ParsePsf2(thin.bytes, kMiniBytes);
    ASSERT_TRUE(!kFont.valid);
}

TEST("font: glyph rows read from their own offset") {
    Psf2Font const kFont = ParsePsf2(kGood.bytes, kMiniBytes);
    ASSERT_TRUE(GlyphRowBits(kGood.bytes, kFont, 0, 0) == 0x80);
    ASSERT_TRUE(GlyphRowBits(kGood.bytes, kFont, 0, 1) == 0x40);
    ASSERT_TRUE(GlyphRowBits(kGood.bytes, kFont, 1, 0) == 0x20);
    ASSERT_TRUE(GlyphRowBits(kGood.bytes, kFont, 1, 1) == 0x10);
}

TEST("font: pixels map MSB-left across the row byte") {
    ASSERT_TRUE(GlyphPixel(0x80, 0));
    ASSERT_TRUE(!GlyphPixel(0x80, 1));
    ASSERT_TRUE(GlyphPixel(0x01, 7));
    ASSERT_TRUE(!GlyphPixel(0x01, 0));
    ASSERT_TRUE(GlyphPixel(0xFF, 3));
}

TEST("font: constants pin the PSF2 grammar") {
    ASSERT_TRUE(cinux::console::kPsf2Magic == 0x864AB572);
    ASSERT_TRUE(cinux::console::kPsf2HeaderBytes == 32);
    ASSERT_TRUE(cinux::console::kPsf2MaxWidth == 8);
}

int main() {
    return cinux::test::RunAll();
}
