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
     * @brief         The boot-reported layout this device adopted.
     *
     * @return        The geometry record, valid for the boot's life.
     * @note          The frame's own facts, not the console grammar's:
     *                physical base, pitch, and the channel masks the
     *                compose math already reads.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] const cinux::boot::FramebufferInfo& layout() const;

    /**
     * @brief         Bytes of pixel memory the device owns.
     *
     * @return        Pitch times height, the mode's whole extent.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long frame_bytes() const;

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

    volatile uint32_t*           base_ = nullptr;
    cinux::boot::FramebufferInfo layout_{};
    uint32_t                     pitch_       = 0;
    uint32_t                     pitch_words_ = 0;
    uint32_t                     width_       = 0;
    uint32_t                     height_      = 0;
    uint32_t                     ink_         = 0;
    uint32_t                     paper_       = 0;
};

}  // namespace cinux::driver
