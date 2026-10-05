#include "kernel/console/font.hpp"

extern "C" {
extern unsigned char g_font_psf_start[];
extern unsigned char g_font_psf_end[];
}

namespace cinux::console {

Psf2Font EmbeddedFont() {
    auto const kBytes =
        // NOLINTNEXTLINE(clang-analyzer-security.PointerSub) linked blob end minus start, one array
        static_cast<unsigned long>(g_font_psf_end - g_font_psf_start);
    return ParsePsf2(g_font_psf_start, kBytes);
}

const unsigned char* EmbeddedFontData() {
    return g_font_psf_start;
}

}  // namespace cinux::console
