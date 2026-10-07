#include "kernel/console/screen.hpp"

#include <stdint.h>

#include "kernel/boot/boot_info.hpp"
#include "kernel/console/console_grid.hpp"
#include "kernel/console/font.hpp"
#include "kernel/driver/framebuffer.hpp"

namespace cinux::console {

bool TextConsole::init(const cinux::boot::FramebufferInfo& info) {
    alive_ = false;
    caret_ = Caret{};
    font_  = EmbeddedFont();
    if (!font_.valid) {
        return false;
    }
    cinux::driver::Framebuffer& screen = cinux::driver::Framebuffer::self();
    if (!screen.init(info)) {
        return false;
    }
    if (font_.width > screen.width() || font_.height > screen.height()) {
        return false;
    }
    screen.clear();
    alive_ = true;
    return true;
}

bool TextConsole::alive() const {
    return alive_;
}

uint32_t TextConsole::columns() const {
    if (!alive_) {
        return 0;
    }
    return cinux::driver::Framebuffer::self().width() / font_.width;
}

uint32_t TextConsole::rows() const {
    if (!alive_) {
        return 0;
    }
    return cinux::driver::Framebuffer::self().height() / font_.height;
}

void TextConsole::put_char(char glyph_char) {
    if (!alive_) {
        return;
    }
    cinux::driver::Framebuffer& screen = cinux::driver::Framebuffer::self();
    uint32_t const              kGlyph =
        (static_cast<uint32_t>(static_cast<unsigned char>(glyph_char)) < font_.glyph_count)
            ? static_cast<uint32_t>(static_cast<unsigned char>(glyph_char))
            : 0;
    if (glyph_char != kCharNewline && glyph_char != kCharReturn && glyph_char != kCharBackspace) {
        paint_glyph(kGlyph, caret_);
    }
    CaretStep const kStep = AdvanceCaret(caret_, columns(), rows(), glyph_char);
    if (kStep.scrolled) {
        screen.scroll_up(font_.height);
    }
    caret_ = kStep.caret;
}

void TextConsole::paint_glyph(uint32_t glyph, Caret cell) {
    const unsigned char*        font_data = EmbeddedFontData();
    cinux::driver::Framebuffer& screen    = cinux::driver::Framebuffer::self();
    uint32_t const              kOriginX  = cell.column * font_.width;
    uint32_t const              kOriginY  = cell.row * font_.height;
    for (uint32_t glyph_row = 0; glyph_row < font_.height; ++glyph_row) {
        unsigned char const kRowBits = GlyphRowBits(font_data, font_, glyph, glyph_row);
        for (uint32_t glyph_column = 0; glyph_column < font_.width; ++glyph_column) {
            screen.put_pixel(kOriginX + glyph_column, kOriginY + glyph_row,
                             GlyphPixel(kRowBits, glyph_column));
        }
    }
}

}  // namespace cinux::console
