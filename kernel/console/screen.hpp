/**
 * @file    screen.hpp
 * @brief   The text screen: one singleton that turns characters into
 *          glyphs on the mapped framebuffer.
 *
 * This is the device half of the console grammar. Caret math lives in
 * console_grid.hpp, pixel plumbing in framebuffer.hpp, glyph bits in
 * font.hpp; here the three meet — one character comes in, pixels move.
 * Before init, or after a refused init, put_char is a quiet no-op, so
 * the neutral console can fan out unconditionally.
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

#include "kernel/boot/boot_info.hpp"
#include "kernel/console/console_grid.hpp"
#include "kernel/console/font.hpp"

namespace cinux::console {

/// The character screen over the framebuffer, as a Meyers singleton.
class TextConsole {
public:
    /**
     * @brief     The one text screen instance.
     *
     * @return    Reference to the Meyers singleton.
     * @since     0.1.0
     * @ingroup   kernel_driver
     */
    static TextConsole& self();

    /**
     * @brief         Adopt the geometry, parse the embedded font, and
     *                clear the screen.
     *
     * @param[in]     info   The framebuffer record from BootInfo.
     * @return        True when both the framebuffer and the font
     *                accepted; false leaves the screen silent.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool init(const cinux::boot::FramebufferInfo& info);

    /**
     * @brief         Whether the screen can paint.
     *
     * @return        True when painting is possible.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool alive() const;

    /**
     * @brief         Text columns the grid offers.
     *
     * @return        Column count.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] uint32_t columns() const;

    /**
     * @brief         Text rows the grid offers.
     *
     * @return        Row count.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] uint32_t rows() const;

    /**
     * @brief         Place one character: paint it, step the caret,
     *                scroll at the bottom. Control characters move the
     *                caret without painting.
     * @param[in]     glyph_char   Character to place.
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void put_char(char glyph_char);

private:
    TextConsole() = default;

    void paint_glyph(uint32_t glyph, Caret cell);

    Caret    caret_{};
    Psf2Font font_{};
    bool     alive_ = false;
};

}  // namespace cinux::console
