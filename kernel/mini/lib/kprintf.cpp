/* ==============================================================
 * Cinux Mini Kernel - Kernel Print Function Implementation
 * ============================================================== */

#include "kprintf.h"
#include "private/format.h"

#include <stdarg.h>
#include <stdint.h>

#include "driver/serial.h"


namespace {

using namespace cinux::mini::lib::detail;

// ============================================================
// Generic Formatted Output
// ============================================================
// OutputFn: void(char) - functor/lambda to output a single character
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc, const char* format, va_list args) {
	char buffer[64];

	while (*format != '\0') {
		if (*format == '%') {
			format++;

			bool zero_pad = false;
			int	 width	  = 0;

			if (*format == '0') {
				zero_pad = true;
				format++;
			}

			while (*format >= '0' && *format <= '9') {
				width = width * 10 + (*format - '0');
				format++;
			}

			char type = *format++;

			int len = 0;

			switch (type) {
			case '%':
				putc('%');
				break;

			case 'c':
				putc(static_cast<char>(va_arg(args, int)));
				break;

			case 's': {
				const char* s = va_arg(args, const char*);
				if (s == nullptr)
					s = "(null)";
				while (*s)
					putc(*s++);
				break;
			}

			case 'd':
				len =
					format_decimal(static_cast<int64_t>(va_arg(args, int)), buffer, sizeof(buffer));
				goto do_padding;

			case 'u':
				len = format_decimal(static_cast<int64_t>(va_arg(args, unsigned int)), buffer,
									 sizeof(buffer));
				goto do_padding;

			case 'x':
				len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), true);
				goto do_padding;

			case 'X':
				len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
				goto do_padding;

			case 'p':
				for (const char* p = "0x"; *p; p++)
					putc(*p);
				len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
				for (int i = 0; i < len; i++)
					putc(buffer[i]);
				break;

			case 'b':
				len = format_binary(va_arg(args, uint64_t), buffer, sizeof(buffer));
				goto do_padding;

do_padding:
				if (len < width) {
					char pad = zero_pad ? '0' : ' ';
					for (int i = width - len; i > 0; i--)
						putc(pad);
				}
				for (int i = 0; i < len; i++)
					putc(buffer[i]);
				break;

			default:
				putc('%');
				putc(type);
				break;
			}
		} else {
			putc(*format++);
		}
	}
}

void debugcon_putc(char c) {
	__asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

}  // namespace


namespace cinux::mini::lib {

// ============================================================
// kprintf - Serial Output
// ============================================================
void kprintf(const char* format, ...) {
	va_list args;
	va_start(args, format);

	auto& serial = serial::get_initial_serial();
	vkprintf_impl([&](char c) { serial.putc(c); }, format, args);

	va_end(args);
}

// ============================================================
// kdebugf - Debug Console Output
// ============================================================
void kdebugf(const char* format, ...) {
	va_list args;
	va_start(args, format);

	vkprintf_impl([](char c) { debugcon_putc(c); }, format, args);

	va_end(args);
}

}  // namespace cinux::mini::lib
