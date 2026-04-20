/**
 * @file kernel/drivers/video/console.cpp
 * @brief Text console driver implementation
 */

#include "console.hpp"

#include <stdint.h>

namespace cinux::drivers {

void Console::init(Framebuffer& fb, PSFFont& font, uint32_t fg, uint32_t bg) {
	fb_	  = &fb;
	font_ = &font;
	fg_	  = fg;
	bg_	  = bg;
	col_  = 0;
	row_  = 0;

	cols_ = fb.width() / font.width();
	rows_ = fb.height() / font.height();

	clear();
}

void Console::putc(char c) {
	if (fb_ == nullptr || font_ == nullptr)
		return;

	switch (c) {
	case '\n':
		new_line();
		break;
	case '\r':
		col_ = 0;
		break;
	case '\b':
		if (col_ > 0) {
			col_--;
		} else if (row_ > 0) {
			row_--;
			col_ = cols_ - 1;
		}
		break;
	default:
		if (col_ >= cols_) {
			new_line();
		}
		font_->render_char(*fb_, static_cast<uint8_t>(c), col_ * font_->width(),
						   row_ * font_->height(), fg_, bg_);
		col_++;
		break;
	}
}

void Console::clear() {
	if (fb_)
		fb_->clear(bg_);
	col_ = 0;
	row_ = 0;
}

void Console::set_color(uint32_t fg, uint32_t bg) {
	fg_ = fg;
	bg_ = bg;
}

void Console::console_sink_adapter(char c, void* ctx) {
	auto* con = static_cast<Console*>(ctx);
	if (con)
		con->putc(c);
}

void Console::new_line() {
	col_ = 0;
	if (row_ + 1 >= rows_) {
		scroll();
	} else {
		row_++;
	}
}

void Console::scroll() {
	if (fb_ == nullptr || font_ == nullptr)
		return;
	uint32_t line_height = font_->height();
	fb_->scroll_up(line_height, line_height, bg_);
}

}  // namespace cinux::drivers
