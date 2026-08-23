/**
 * @file detail/vformat.cpp
 * @brief Buffer-writing wrapper for the vformat engine.
 */

#include <cinux/detail/vformat.hpp>
#include <cstdarg>
#include <cstddef>

namespace cinux::lib::detail {

void vformat_to_buf(char* buf, size_t buf_size, const char* fmt, va_list args) {
    if (!buf || buf_size == 0) {
        return;
    }

    size_t  pos = 0;
    va_list args_copy;
    va_copy(args_copy, args);

    vformat_impl(
        [&](char c) {
            if (pos + 1 < buf_size) {
                buf[pos++] = c;
            }
        },
        fmt, args_copy);

    buf[pos] = '\0';
    va_end(args_copy);
}

}  // namespace cinux::lib::detail
