/**
 * @file    framebuffer.hpp
 * @brief   The linear framebuffer: color grammar plus one singleton
 *          device the boot world has already mapped into reach.
 *
 * The math — composing a pixel from channel masks, finding a pixel's
 * byte offset — is constexpr and host-testable. The device side is a
 * Meyers singleton fed from the BootInfo record; writing goes through
 * the identity mapping the boot page tables grant, so no paging work
 * happens here.
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

#include "cinux/singleton.hpp"
#include "kernel/boot/boot_info.hpp"

namespace cinux::driver {

/// The one color this console grammar speaks: paint or erase.
struct Paint {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

/// White on the common XRGB layout.
inline constexpr Paint kInk{.red = 0xFF, .green = 0xFF, .blue = 0xFF};

/// The backdrop every glyph hole gets.
inline constexpr Paint kPaper{.red = 0x00, .green = 0x00, .blue = 0x00};

/**
 * @brief         Compose one 32-bit pixel from a channel layout and a
 *                paint, truncating each channel to its mask width.
 *
 * @param[in]     layout   The framebuffer's color field facts.
 * @param[in]     paint    The color to lay down.
 * @return        The pixel as the display expects it.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
constexpr uint32_t ComposePixel(const cinux::boot::FramebufferInfo& layout, Paint paint) {
    uint32_t const kRed   = static_cast<uint32_t>(paint.red) >> (8U - layout.red_size);
    uint32_t const kGreen = static_cast<uint32_t>(paint.green) >> (8U - layout.green_size);
    uint32_t const kBlue  = static_cast<uint32_t>(paint.blue) >> (8U - layout.blue_size);
    return (kRed << layout.red_shift) | (kGreen << layout.green_shift) |
           (kBlue << layout.blue_shift);
}

/**
 * @brief         Byte offset of one pixel in a 32bpp linear framebuffer.
 *
 * @param[in]     pitch    Row stride in bytes; width times four, rounded
 *                         by the hardware.
 * @param[in]     column   Pixel column, zero at the left edge.
 * @param[in]     row      Pixel row, zero at the top edge.
 * @return        Byte offset from the framebuffer base.
 * @warning       Undefined past the edges; callers own the bounds.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
constexpr uint64_t PixelByteOffset(uint32_t pitch, uint32_t column, uint32_t row) {
    return (static_cast<uint64_t>(row) * pitch) + (static_cast<uint64_t>(column) * 4U);
}

/// The mapped linear framebuffer as a singleton device.
class Framebuffer : public cinux::base::Singleton<Framebuffer> {
    friend class cinux::base::Singleton<Framebuffer>;

public:
    /**
     * @brief         Adopt the boot-reported geometry and masks.
     *
     * @param[in]     info   The framebuffer record from BootInfo.
     * @return        True when the geometry is usable (32bpp, non-zero
     *                extents); false leaves the device inert.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool init(const cinux::boot::FramebufferInfo& info);

    /**
     * @brief         Whether init accepted a usable geometry.
     *
     * @return        True when the device can paint.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool alive() const;

    /**
     * @brief         Columns of pixels.
     *
     * @return        Width in pixels.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] uint32_t width() const;

    /**
     * @brief         Rows of pixels.
     *
     * @return        Height in pixels.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] uint32_t height() const;

    /**
     * @brief         Paint or erase one pixel; erase writes the paper
     *                color.
     *
     * @param[in]     column   Pixel column.
     * @param[in]     row      Pixel row.
     * @param[in]     lit      Paint when true, erase when false.
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void put_pixel(uint32_t column, uint32_t row, bool lit);

    /**
     * @brief         Flood the whole surface with the paper color.
     *
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void clear();

    /**
     * @brief         Shift every pixel row up by line_count, painting
     *                the freed band at the bottom with the paper color.
     *
     * @param[in]     line_count   Pixel rows to shift up.
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void scroll_up(uint32_t line_count);

private:
    Framebuffer() = default;

    volatile uint32_t* address_of(uint32_t column, uint32_t row);

    volatile uint32_t* base_        = nullptr;
    uint32_t           pitch_       = 0;
    uint32_t           pitch_words_ = 0;
    uint32_t           width_       = 0;
    uint32_t           height_      = 0;
    uint32_t           ink_         = 0;
    uint32_t           paper_       = 0;
};

}  // namespace cinux::driver
