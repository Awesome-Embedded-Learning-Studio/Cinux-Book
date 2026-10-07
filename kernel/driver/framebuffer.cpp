#include "kernel/driver/framebuffer.hpp"

#include <stdint.h>

#include "cinux/ptr.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/mm/layout.hpp"

namespace cinux::driver {

bool Framebuffer::init(const cinux::boot::FramebufferInfo& info) {
    if (info.bpp != 32 || info.width == 0 || info.height == 0 || info.pitch < info.width * 4U) {
        return false;
    }
    base_ = cinux::base::PtrAt<volatile uint32_t>(
        cinux::mm::IoremapVirt(static_cast<unsigned long>(info.physical)));
    pitch_       = info.pitch;
    pitch_words_ = info.pitch / 4U;
    width_       = info.width;
    height_      = info.height;
    ink_         = ComposePixel(info, kInk);
    paper_       = ComposePixel(info, kPaper);
    return true;
}

bool Framebuffer::alive() const {
    return base_ != nullptr;
}

uint32_t Framebuffer::width() const {
    return width_;
}

uint32_t Framebuffer::height() const {
    return height_;
}

void Framebuffer::put_pixel(uint32_t column, uint32_t row, bool lit) {
    if (base_ == nullptr || column >= width_ || row >= height_) {
        return;
    }
    *address_of(column, row) = lit ? ink_ : paper_;
}

void Framebuffer::clear() {
    if (base_ == nullptr) {
        return;
    }
    for (uint32_t row = 0; row < height_; ++row) {
        volatile uint32_t* line = base_ + (static_cast<uint64_t>(row) * pitch_words_);
        for (uint32_t word = 0; word < pitch_words_; ++word) {
            line[word] = paper_;
        }
    }
}

void Framebuffer::scroll_up(uint32_t line_count) {
    if (base_ == nullptr) {
        return;
    }
    if (line_count >= height_) {
        clear();
        return;
    }
    uint32_t const kRowsToMove = height_ - line_count;
    for (uint32_t row = 0; row < kRowsToMove; ++row) {
        const volatile uint32_t* source =
            base_ + (static_cast<uint64_t>(row + line_count) * pitch_words_);
        volatile uint32_t* target = base_ + (static_cast<uint64_t>(row) * pitch_words_);
        for (uint32_t word = 0; word < pitch_words_; ++word) {
            target[word] = source[word];
        }
    }
    for (uint32_t row = kRowsToMove; row < height_; ++row) {
        volatile uint32_t* freed = base_ + (static_cast<uint64_t>(row) * pitch_words_);
        for (uint32_t word = 0; word < pitch_words_; ++word) {
            freed[word] = paper_;
        }
    }
}

volatile uint32_t* Framebuffer::address_of(uint32_t column, uint32_t row) {
    return reinterpret_cast<volatile uint32_t*>(reinterpret_cast<volatile unsigned char*>(base_) +
                                                PixelByteOffset(pitch_, column, row));
}

}  // namespace cinux::driver
