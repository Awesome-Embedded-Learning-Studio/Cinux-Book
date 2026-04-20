/**
 * @file kernel/drivers/video/console.hpp
 * @brief Text console driver backed by a framebuffer + PSF2 font
 *
 * Provides a VT100-like text console that renders characters, handles
 * line wrapping, scrolling, and basic control characters (\n, \r, \b).
 * Exposes a static sink adapter for registration with kprintf.
 *
 * Usage:
 *   Console console;
 *   console.init(fb, font, 0x00FFFFFF, 0x00000000);
 *   kprintf_register_sink(Console::console_sink_adapter, &console);
 *   kprintf("Hello, screen!\n");  // appears on both serial and screen
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include "font.hpp"
#include "framebuffer.hpp"

namespace cinux::drivers {

class Console {
public:
	/**
	 * @brief Initialise the console with a framebuffer and font
	 *
	 * @param fb   Framebuffer to render to
	 * @param font PSF2 font for character rendering
	 * @param fg   Default foreground colour (0x00RRGGBB)
	 * @param bg   Default background colour (0x00RRGGBB)
	 */
	void init(Framebuffer& fb, PSFFont& font, uint32_t fg, uint32_t bg);

	/**
	 * @brief Write a single character to the console
	 *
	 * Handles control characters:
	 *   \\n  -- new line (CR + LF)
	 *   \\r  -- carriage return (move to column 0)
	 *   \\b  -- backspace (move cursor left, wrap if needed)
	 *
	 * Printable characters are rendered at the current cursor position
	 * and the cursor advances.  Auto-wraps at the right margin.
	 *
	 * @param c  Character to write
	 */
	void putc(char c);

	/**
	 * @brief Clear the console and reset cursor to home
	 */
	void clear();

	/**
	 * @brief Set the foreground and background colours
	 *
	 * @param fg  Foreground colour (0x00RRGGBB)
	 * @param bg  Background colour (0x00RRGGBB)
	 */
	void set_color(uint32_t fg, uint32_t bg);

	/**
	 * @brief Sink adapter for kprintf multi-backend registration
	 *
	 * @param c    Character to write
	 * @param ctx  Pointer to the Console instance (passed as void*)
	 */
	static void console_sink_adapter(char c, void* ctx);

private:
	void scroll();
	void new_line();

	Framebuffer* fb_   = nullptr;
	PSFFont*	 font_ = nullptr;
	uint32_t	 col_  = 0;
	uint32_t	 row_  = 0;
	uint32_t	 cols_ = 0;
	uint32_t	 rows_ = 0;
	uint32_t	 fg_   = 0x00FFFFFF;
	uint32_t	 bg_   = 0x00000000;
};

}  // namespace cinux::drivers
