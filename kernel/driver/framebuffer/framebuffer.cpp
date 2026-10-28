#include "kernel/driver/framebuffer/framebuffer.hpp"

#include <stdint.h>

#include "cinux/ptr.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/mm/layout.hpp"

namespace cinux::driver {

namespace {

/// Compose one 32-bit pixel from a channel layout and a paint,
/// truncating each channel to its mask width.
constexpr uint32_t compose_pixel(const cinux::boot::FramebufferInfo& layout, Paint paint) {
    uint32_t const kRed   = static_cast<uint32_t>(paint.red) >> (8U - layout.red_size);
    uint32_t const kGreen = static_cast<uint32_t>(paint.green) >> (8U - layout.green_size);
    uint32_t const kBlue  = static_cast<uint32_t>(paint.blue) >> (8U - layout.blue_size);
    return (kRed << layout.red_shift) | (kGreen << layout.green_shift) |
           (kBlue << layout.blue_shift);
}

/// Byte offset of one pixel in a 32bpp linear framebuffer; callers own
/// the bounds.
constexpr uint64_t pixel_byte_offset(uint32_t pitch, uint32_t column, uint32_t row) {
    return (static_cast<uint64_t>(row) * pitch) + (static_cast<uint64_t>(column) * 4U);
}

}

bool Framebuffer::init(const cinux::boot::FramebufferInfo& info) {
    if (info.bpp != 32 || info.width == 0 || info.height == 0 || info.pitch < info.width * 4U) {
        return false;
    }
    base_ = cinux::base::PtrAt<volatile uint32_t>(
        cinux::mm::IoremapVirt(static_cast<unsigned long>(info.physical)));
    layout_      = info;
    pitch_       = info.pitch;
    pitch_words_ = info.pitch / 4U;
    width_       = info.width;
    height_      = info.height;
    ink_         = compose_pixel(info, kInk);
    paper_       = compose_pixel(info, kPaper);
    return true;
}

const cinux::boot::FramebufferInfo& Framebuffer::layout() const {
    return layout_;
}

unsigned long Framebuffer::frame_bytes() const {
    return static_cast<unsigned long>(pitch_) * height_;
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
                                                pixel_byte_offset(pitch_, column, row));
}

}  // namespace cinux::driver
